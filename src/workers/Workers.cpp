/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2016-2017 XMRig       <support@xmrig.com>
 *
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <cmath>

#ifdef __GNUC__
#   include <mm_malloc.h>
#else
#   include <malloc.h>
#endif


#include "amd/OclGPU.h"
#include "api/Api.h"
#include "crypto/CryptoNight.h"
#include "interfaces/IJobResultListener.h"
#include "log/Log.h"
#include "Options.h"
#include "workers/OclWorker.h"
#include "workers/Handle.h"
#include "workers/Hashrate.h"
#include "workers/OclThread.h"
#include "workers/Workers.h"


bool Workers::m_active = false;
bool Workers::m_enabled = true;
Hashrate *Workers::m_hashrate = nullptr;
IJobResultListener *Workers::m_listener = nullptr;
Job Workers::m_job;
std::atomic<int> Workers::m_paused;
std::atomic<uint64_t> Workers::m_sequence;
std::list<Job> Workers::m_queue;
std::vector<Handle*> Workers::m_workers;
uint64_t Workers::m_ticks = 0;
uv_async_t Workers::m_async;
uv_mutex_t Workers::m_mutex;
uv_rwlock_t Workers::m_rwlock;
uv_timer_t Workers::m_reportTimer;
uv_timer_t Workers::m_timer;


static std::vector<GpuContext> contexts;


struct JobBaton
{
    uv_work_t request;
    std::vector<Job> jobs;
    std::vector<JobResult> results;
    int errors = 0;

    JobBaton() {
        request.data = this;
    }
};



bool Workers::start(const std::vector<OclThread*> &threads)
{
    const size_t count = threads.size();
    m_hashrate = new Hashrate((int) count);

    if (count == 0) {
        return false;
    }

    uv_mutex_init(&m_mutex);
    uv_rwlock_init(&m_rwlock);

    m_sequence = 1;
    m_paused   = 1;

    uv_async_init(uv_default_loop(), &m_async, Workers::onResult);

    contexts.resize(count);

    for (size_t i = 0; i < count; ++i) {
        const OclThread *thread = threads[i];
        contexts[i] = GpuContext(thread->index(), thread->intensity(), thread->worksize());
    }

    if (InitOpenCL(contexts.data(), count, Options::i()->platformIndex()) != OCL_ERR_SUCCESS) {
        return false;
    }

    for (size_t i = 0; i < count; ++i) {
        Handle *handle = new Handle((int) i, threads[i], &contexts[i], (int) count, Options::i()->algo() == Options::ALGO_CRYPTONIGHT_LITE);
        m_workers.push_back(handle);
        handle->start(Workers::onReady);
    }

    uv_timer_init(uv_default_loop(), &m_timer);
    uv_timer_start(&m_timer, Workers::onTick, 500, 500);
 
    const int printTime = Options::i()->printTime();
    if (printTime > 0) {
        uv_timer_init(uv_default_loop(), &m_reportTimer);
        uv_timer_start(&m_reportTimer, Workers::onReport, (printTime + 4) * 1000, printTime * 1000);
    }

    Options::i()->save();
    return true;
}


Job Workers::job()
{
    uv_rwlock_rdlock(&m_rwlock);
    Job job = m_job;
    uv_rwlock_rdunlock(&m_rwlock);

    return job;
}


void Workers::printHashrate(bool detail)
{
    if (detail) {
       for (const OclThread *thread : Options::i()->threads()) {
            m_hashrate->print(thread->threadId(), thread->index());
        }
    }

    m_hashrate->print();
}


void Workers::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }

    m_enabled = enabled;
    if (!m_active) {
        return;
    }

    m_paused = enabled ? 0 : 1;
    m_sequence++;
}


void Workers::setJob(const Job &job)
{
    uv_rwlock_wrlock(&m_rwlock);
    m_job = job;
    uv_rwlock_wrunlock(&m_rwlock);

    m_active = true;
    if (!m_enabled) {
        return;
    }

    m_sequence++;
    m_paused = 0;
}


void Workers::stop()
{
    if (Options::i()->printTime() > 0) {
        uv_timer_stop(&m_reportTimer);
    }

    uv_timer_stop(&m_timer);
    m_hashrate->stop();

    uv_close(reinterpret_cast<uv_handle_t*>(&m_async), nullptr);
    m_paused   = 0;
    m_sequence = 0;

    for (size_t i = 0; i < m_workers.size(); ++i) {
        m_workers[i]->join();
    }
}


void Workers::submit(const Job &result)
{
    uv_mutex_lock(&m_mutex);
    m_queue.push_back(result);
    uv_mutex_unlock(&m_mutex);

    uv_async_send(&m_async);
}


void Workers::onReady(void *arg)
{
    auto handle = static_cast<Handle*>(arg);
    handle->setWorker(new OclWorker(handle));

    handle->worker()->start();
}


void Workers::onResult(uv_async_t *handle)
{
    JobBaton *baton = new JobBaton();

    uv_mutex_lock(&m_mutex);
    while (!m_queue.empty()) {
        baton->jobs.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
    }
    uv_mutex_unlock(&m_mutex);

    uv_queue_work(uv_default_loop(), &baton->request,
        [](uv_work_t* req) {
            JobBaton *baton = static_cast<JobBaton*>(req->data);
            cryptonight_ctx *ctx = static_cast<cryptonight_ctx*>(_mm_malloc(sizeof(cryptonight_ctx), 16));

            for (const Job &job : baton->jobs) {
                JobResult result(job);

                if (CryptoNight::hash(job, result, ctx)) {
                    baton->results.push_back(result);
                }
                else {
                    baton->errors++;
                }
            }

            _mm_free(ctx);
        },
        [](uv_work_t* req, int status) {
            JobBaton *baton = static_cast<JobBaton*>(req->data);

            for (const JobResult &result : baton->results) {
                m_listener->onJobResult(result);
            }

            if (baton->errors > 0 && !baton->jobs.empty()) {
                LOG_ERR("GPU #%d COMPUTE ERROR", baton->jobs[0].threadId());
            }

            delete baton;
        }
    );
}


void Workers::onReport(uv_timer_t *handle)
{
    m_hashrate->print();
}


void Workers::onTick(uv_timer_t *handle)
{
    for (Handle *handle : m_workers) {
        if (!handle->worker()) {
            return;
        }

        m_hashrate->add(handle->threadId(), handle->worker()->hashCount(), handle->worker()->timestamp());
    }

    if ((m_ticks++ & 0xF) == 0)  {
        m_hashrate->updateHighest();
    }

#   ifndef XMRIG_NO_API
    Api::tick(m_hashrate);
#   endif
}
