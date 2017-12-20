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

#ifndef __OCLWORKER_H__
#define __OCLWORKER_H__

#include <atomic>


#include "interfaces/IWorker.h"
#include "net/Job.h"
#include "net/JobResult.h"


class Handle;
struct GpuContext;


class OclWorker : public IWorker
{
public:
    OclWorker(Handle *handle);

protected:
    inline uint64_t hashCount() const override { return m_hashCount.load(std::memory_order_relaxed); }
    inline uint64_t timestamp() const override { return m_timestamp.load(std::memory_order_relaxed); }

    void start() override;

private:
    bool resume(const Job &job);
    void consumeJob();
    void save(const Job &job);
    void storeStats();

    bool m_lite;
    const int m_id;
    const int m_threads;
    GpuContext *m_ctx;
    Job m_job;
    Job m_pausedJob;
    std::atomic<uint64_t> m_hashCount;
    std::atomic<uint64_t> m_timestamp;
    uint32_t m_nonce;
    uint32_t m_pausedNonce;
    uint64_t m_count;
    uint64_t m_sequence;
};


#endif /* __OCLWORKER_H__ */
