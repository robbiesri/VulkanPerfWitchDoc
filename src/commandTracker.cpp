#include <assert.h>

#include "commandTracker.h"


CommandPoolAndBufferTracker::CommandPoolAndBufferTracker()
{
    m_trackedPools.clear();
    m_trackedCommandBuffers.clear();
    m_poolToBuffersMap.clear();
}

CommandPoolAndBufferTracker::~CommandPoolAndBufferTracker()
{
    m_trackedPools.clear();
    m_trackedCommandBuffers.clear();

    for (auto& poolMapPair : m_poolToBuffersMap)
    {
        poolMapPair.second.clear();
    }
    m_poolToBuffersMap.clear();
}

void CommandPoolAndBufferTracker::AddPool(VkCommandPool pool)
{
    m_mutex.lock();
    m_trackedPools.insert(pool);
    m_mutex.unlock();
}

void CommandPoolAndBufferTracker::RemovePool(VkCommandPool pool)
{
    RemovePoolAssociatedCommandBuffers(pool);
    m_mutex.lock();
    m_trackedPools.erase(pool);
    m_mutex.unlock();
}

void CommandPoolAndBufferTracker::AddCommandBuffers(VkCommandPool pool, const VkCommandBuffer * pCommandBuffers, uint32_t count)
{
    m_mutex.lock();
    if (m_trackedPools.find(pool) != m_trackedPools.end())
    {
        CommandBufferSet& poolAssociatedCmdBufs = m_poolToBuffersMap[pool];
        for (uint32_t idx = 0; idx < count; idx++)
        {
            const VkCommandBuffer& cmdBuf = pCommandBuffers[idx];
            assert(cmdBuf != VK_NULL_HANDLE);

            if (cmdBuf != VK_NULL_HANDLE)
            {
                poolAssociatedCmdBufs.insert(cmdBuf);
                m_trackedCommandBuffers.insert(cmdBuf);
            }
        }
    }
    m_mutex.unlock();
}

void CommandPoolAndBufferTracker::RemoveCommandBuffers(VkCommandPool pool, const VkCommandBuffer * pCommandBuffers, uint32_t count)
{
    m_mutex.lock();
    if (m_poolToBuffersMap.find(pool) != m_poolToBuffersMap.end())
    {
        CommandBufferSet& poolAssociatedCmdBufs = m_poolToBuffersMap[pool];

        for (uint32_t idx = 0; idx < count; idx++)
        {
            const VkCommandBuffer& cmdBuf = pCommandBuffers[idx];
            m_trackedCommandBuffers.erase(cmdBuf);
            poolAssociatedCmdBufs.erase(cmdBuf);
        }
    }
    m_mutex.unlock();
}

bool CommandPoolAndBufferTracker::IsCommandBufferTracked(VkCommandBuffer commandBuffer)
{
    return (m_trackedCommandBuffers.find(commandBuffer) != m_trackedCommandBuffers.end());
}

void CommandPoolAndBufferTracker::RemovePoolAssociatedCommandBuffers(VkCommandPool pool)
{
    // TODO: Verify set deletion
    m_mutex.lock();
    if (m_poolToBuffersMap.find(pool) != m_poolToBuffersMap.end())
    {
        CommandBufferSet& cmdBufsToRemove = m_poolToBuffersMap[pool];
        for (VkCommandBuffer cmdBuf : cmdBufsToRemove)
        {
            m_trackedCommandBuffers.erase(cmdBuf);
        }
        m_poolToBuffersMap[pool].clear();
        m_poolToBuffersMap.erase(pool);
    }
    m_mutex.unlock();
}

bool CommandPoolAndBufferTracker::IsCommandPoolTracked(VkCommandPool pool)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return (m_poolToBuffersMap.find(pool) != m_poolToBuffersMap.end());
}

CommandBufferSet& CommandPoolAndBufferTracker::GetPoolAssociatedCommandBuffers(VkCommandPool pool)
{
    // TODO: Make this a unique pointer
    return m_poolToBuffersMap[pool];
}
