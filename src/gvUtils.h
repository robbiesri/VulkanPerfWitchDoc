#pragma once

#include <vulkan/vulkan.h>

#include <mutex>
#include <unordered_set>
#include <vector>

struct QueueFamilyInfoManager
{
    std::vector<VkQueueFamilyProperties>  m_queueFamilyPropsList;

    std::unordered_set<uint32_t> m_timestampQFIndices;
    std::unordered_set<uint32_t> m_resetQueryPoolQFIndices;

    void BuildQFInfoState(std::vector<VkQueueFamilyProperties>& qfPropertiesList);

    VkQueueFlags GetQueueFamilyQueueFlags(uint32_t qfIndex);
    bool DoesQFSupportTimestamps(uint32_t qfIndex);
    bool DoesQFSupportTimestampResets(uint32_t qfIndex);
};

////////////////////////////////

// TODO: Pick a proper slot count. I tested 1024 with Paladin, and it
// worked fine (woo!). The massive slot counts are needed during loading.
// Perhaps we can make the slot manager choose it's size on construction?
const uint32_t kNumLogicalQuerySlots = 16384; // TODO: Temp boost while re-factoring slot manager
const uint32_t kNumPhysicalTimerQuerySlots = kNumLogicalQuerySlots * 2;
const uint32_t kNumPhysicalStatQuerySlots = kNumLogicalQuerySlots;

typedef std::pair<uint32_t, uint32_t> SlotRange;
typedef std::vector<SlotRange> SlotRangeList;

struct QuerySlotManager
{
    // potential slot states
    // 0. freshly created, who cares
    // 1. Reset completed, ready for query  issue
    // 2. Query in flight, pending GPU completion
    // 3. Query completed, ready for readback
    // 4. Data readback, ready for reset
    // 5. Reset issued, pending GPU completion
    //    (loop back to 1)
    // As far as the future slot manager is concerned, 2/3 are combined.
    // The client is responsible for checking on completion because it has the submit
    // info. This could be re-arranged so the list of submits is given to the slot manager to
    // update the timestamp completion state.
    enum SlotState {
        ReadyForQueryIssue = 0,
        QueryPendingOnGPU,
        QueryReadbackReady,
        ReadyForResetIssue,
        ResetPendingOnGPU,
        Count,
    };

    // TODO: Think about how to make the slot manager more multi-threaded
    // performant. The first thought I have is to allocate sub-blocks on a
    // per thread basis. So each encountered thread would have a current
    // block of slots allocated that it would iterate through.
    // Actually, this could work. The only 'mutexed' time would be the
    // allocation of the block. Once we have the block, we can do
    // whatever we like without checking back in until we're done with
    // the block. Maybe if the block size is substantial, we can contain
    // all operations to one block, never synchronizing.

    std::mutex m_slotMutex;
    volatile SlotState m_slotState[kNumLogicalQuerySlots];
    volatile uint32_t m_nextFreeIndex = 0;

    QuerySlotManager();
    ~QuerySlotManager() {}

    bool GetNextReadyQuerySlot(uint32_t& allocatedIndex);
    void MarkSlots(std::vector<uint32_t>& slotsToMark, SlotState newState);
    void BuildMatchingSlotList(std::vector<uint32_t>& slots, SlotState matchState);
    //void MarkSlots(SlotRangeList& slotsToMark, SlotState newState); // TODO: More efficient implementation

    void RollBackSlots(std::vector<uint32_t>& slotsToMark, SlotState rollbackState);

    // TODO: Make this debug-build-only
    uint32_t m_numFreeSlots;
    uint32_t m_numActiveSlots;

    // TODO: Build ranges from slot state in order to minimize reset commands
    // TODO: Accelerate lookups with bitfields
};