// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <list>

#include "common/assert.h"
#include "common/common_types.h"
#include "core/hle/kernel/memory/memory_types.h"
#include "core/hle/result.h"

namespace Kernel::Memory {

class PageLinkedList final {
public:
    class Node final {
    public:
        constexpr Node(u64 addr, std::size_t num_pages) : addr{addr}, num_pages{num_pages} {}

        constexpr u64 GetAddress() const {
            return addr;
        }

        constexpr std::size_t GetNumPages() const {
            return num_pages;
        }

    private:
        u64 addr{};
        std::size_t num_pages{};
    };

public:
    PageLinkedList() = default;
    PageLinkedList(u64 address, u64 num_pages) {
        ASSERT(AddBlock(address, num_pages).IsSuccess());
    }

    constexpr std::list<Node>& Nodes() {
        return nodes;
    }

    constexpr const std::list<Node>& Nodes() const {
        return nodes;
    }

    std::size_t GetNumPages() const {
        std::size_t num_pages = 0;
        for (const Node& node : nodes) {
            num_pages += node.GetNumPages();
        }
        return num_pages;
    }

    bool IsEqual(PageLinkedList& other) const {
        auto this_node = nodes.begin();
        auto other_node = other.nodes.begin();
        while (this_node != nodes.end() && other_node != other.nodes.end()) {
            if (this_node->GetAddress() != other_node->GetAddress() ||
                this_node->GetNumPages() != other_node->GetNumPages()) {
                return false;
            }
            this_node = std::next(this_node);
            other_node = std::next(other_node);
        }

        return this_node == nodes.end() && other_node == other.nodes.end();
    }

    ResultCode AddBlock(u64 address, u64 num_pages) {
        if (!num_pages) {
            return RESULT_SUCCESS;
        }
        if (!nodes.empty()) {
            const auto node = nodes.back();
            if (node.GetAddress() + node.GetNumPages() * PageSize == address) {
                address = node.GetAddress();
                num_pages += node.GetNumPages();
                nodes.pop_back();
            }
        }
        nodes.push_back({address, num_pages});
        return RESULT_SUCCESS;
    }

private:
    std::list<Node> nodes;
};

} // namespace Kernel::Memory
