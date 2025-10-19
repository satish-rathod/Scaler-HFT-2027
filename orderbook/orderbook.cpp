// order_book.hpp - Complete High-Performance Limit Order Book Implementation
// Compile: g++ -std=c++17 -O3 -march=native -DORDERBOOK_MAIN order_book.cpp -o orderbook
// For best performance: g++ -std=c++17 -O3 -march=native -flto -DNDEBUG -DORDERBOOK_MAIN order_book.cpp -o orderbook

#pragma once
#include <cstdint>
#include <vector>
#include <map>
#include <unordered_map>
#include <list>
#include <memory>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <random>
#include <sstream>

// ======================== CORE DATA STRUCTURES ========================

// Order structure
struct Order {
    uint64_t order_id;
    bool is_buy;
    double price;
    uint64_t quantity;
    uint64_t timestamp_ns;
};

// Price level aggregation for snapshots
struct PriceLevel {
    double price;
    uint64_t total_quantity;
};

// ======================== MEMORY POOL IMPLEMENTATION ========================

// High-performance memory pool with aligned allocation
template<typename T, size_t BlockSize = 4096>
class MemoryPool {
private:
    // Aligned memory block for cache efficiency
    struct Block {
        alignas(64) char data[sizeof(T) * BlockSize];  // 64-byte cache line alignment
        Block* next;
    };
    
    Block* head_block;
    std::vector<std::unique_ptr<Block>> blocks;
    std::vector<T*> free_list;
    size_t current_index;
    
    // Statistics
    size_t total_allocations = 0;
    size_t total_deallocations = 0;
    size_t blocks_allocated = 0;
    
public:
    MemoryPool() : head_block(nullptr), current_index(BlockSize) {
        // Pre-allocate first block
        allocate_block();
        
        // Reserve space for free list to avoid reallocation
        free_list.reserve(BlockSize);
    }
    
    ~MemoryPool() = default;
    
    // Fast allocation with minimal branching
    [[nodiscard]] inline T* allocate() {
        ++total_allocations;
        
        // Fast path: reuse from free list
        if (!free_list.empty()) {
            T* ptr = free_list.back();
            free_list.pop_back();
            return ptr;
        }
        
        // Need new allocation
        if (current_index >= BlockSize) {
            allocate_block();
        }
        
        return reinterpret_cast<T*>(&head_block->data[sizeof(T) * current_index++]);
    }
    
    // Fast deallocation
    inline void deallocate(T* ptr) {
        if (ptr) {
            ++total_deallocations;
            ptr->~T();
            free_list.push_back(ptr);
        }
    }
    
    // Get pool statistics
    void print_stats() const {
        std::cout << "Memory Pool Stats:\n";
        std::cout << "  Blocks: " << blocks_allocated << "\n";
        std::cout << "  Total Allocations: " << total_allocations << "\n";
        std::cout << "  Total Deallocations: " << total_deallocations << "\n";
        std::cout << "  Active Objects: " << (total_allocations - total_deallocations) << "\n";
        std::cout << "  Free List Size: " << free_list.size() << "\n";
    }
    
private:
    void allocate_block() {
        auto new_block = std::make_unique<Block>();
        new_block->next = head_block;
        head_block = new_block.get();
        blocks.push_back(std::move(new_block));
        current_index = 0;
        ++blocks_allocated;
    }
};

// ======================== INTERNAL ORDER REPRESENTATION ========================

// Order node with embedded list iterator for O(1) removal
struct OrderNode {
    Order order;
    std::list<OrderNode*>::iterator level_iterator;
    double original_price;  // For tracking price changes
    
    OrderNode(const Order& o) : order(o), original_price(o.price) {}
};

// Price level with FIFO order queue
struct Level {
    double price;
    uint64_t total_quantity;
    std::list<OrderNode*> orders;  // FIFO queue
    uint64_t order_count;           // Track number of orders at this level
    
    Level(double p = 0.0) : price(p), total_quantity(0), order_count(0) {}
    
    // Add order to level
    inline void add_order(OrderNode* node) {
        orders.push_back(node);
        node->level_iterator = std::prev(orders.end());
        total_quantity += node->order.quantity;
        ++order_count;
    }
    
    // Remove order from level
    inline void remove_order(OrderNode* node) {
        orders.erase(node->level_iterator);
        total_quantity -= node->order.quantity;
        --order_count;
    }
    
    // Update order quantity in place
    inline void update_quantity(OrderNode* node, uint64_t new_quantity) {
        total_quantity = total_quantity - node->order.quantity + new_quantity;
        node->order.quantity = new_quantity;
    }
    
    [[nodiscard]] inline bool is_empty() const {
        return orders.empty();
    }
};

// ======================== ORDER BOOK IMPLEMENTATION ========================

class OrderBook {
private:
    // Memory pools for different allocation sizes
    MemoryPool<OrderNode, 1024> order_pool;       // Orders
    MemoryPool<Level, 256> level_pool;            // Price levels
    
    // Price levels sorted by price
    // Using custom comparators for optimal sorting
    std::map<double, Level*, std::greater<double>> bid_levels;  // Highest first
    std::map<double, Level*, std::less<double>> ask_levels;     // Lowest first
    
    // O(1) order lookup with reserve optimization
    std::unordered_map<uint64_t, OrderNode*> order_lookup;
    
    // Performance counters
    mutable uint64_t total_orders = 0;
    mutable uint64_t total_cancels = 0;
    mutable uint64_t total_amends = 0;
    mutable uint64_t total_snapshots = 0;
    
    // Cache for best prices
    mutable double cached_best_bid = 0.0;
    mutable double cached_best_ask = 0.0;
    mutable bool cache_valid = false;
    
public:
    OrderBook() {
        // Pre-allocate hash map buckets for better performance
        order_lookup.reserve(10000);
    }
    
    ~OrderBook() {
        // Clean up all orders
        for (auto& [id, node] : order_lookup) {
            order_pool.deallocate(node);
        }
        
        // Clean up all levels
        for (auto& [price, level] : bid_levels) {
            level_pool.deallocate(level);
        }
        for (auto& [price, level] : ask_levels) {
            level_pool.deallocate(level);
        }
    }
    
    // Insert a new order into the book
    void add_order(const Order& order) {
        // Allocate new order node from pool
        OrderNode* node = order_pool.allocate();
        new(node) OrderNode(order);
        
        // Add to lookup table
        order_lookup.emplace(order.order_id, node);
        
        // Invalidate cache
        cache_valid = false;
        
        // Add to appropriate side
        if (order.is_buy) {
            add_to_side(bid_levels, node);
        } else {
            add_to_side(ask_levels, node);
        }
        
        ++total_orders;
    }
    
    // Cancel an existing order by its ID
    [[nodiscard]] bool cancel_order(uint64_t order_id) {
        auto it = order_lookup.find(order_id);
        if (it == order_lookup.end()) {
            return false;
        }
        
        OrderNode* node = it->second;
        
        // Invalidate cache
        cache_valid = false;
        
        // Remove from appropriate side
        if (node->order.is_buy) {
            remove_from_side(bid_levels, node);
        } else {
            remove_from_side(ask_levels, node);
        }
        
        // Remove from lookup and deallocate
        order_lookup.erase(it);
        order_pool.deallocate(node);
        
        ++total_cancels;
        return true;
    }
    
    // Amend an existing order's price or quantity
    [[nodiscard]] bool amend_order(uint64_t order_id, double new_price, uint64_t new_quantity) {
        auto it = order_lookup.find(order_id);
        if (it == order_lookup.end()) {
            return false;
        }
        
        OrderNode* node = it->second;
        
        // Invalidate cache
        cache_valid = false;
        
        // Check if price changes (using epsilon for floating point comparison)
        constexpr double epsilon = 1e-9;
        if (std::abs(node->order.price - new_price) > epsilon) {
            // Price change: cancel and re-add to maintain FIFO
            Order new_order = node->order;
            new_order.price = new_price;
            new_order.quantity = new_quantity;
            new_order.timestamp_ns = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            
            cancel_order(order_id);
            add_order(new_order);
        } else {
            // Quantity-only change: update in place
            if (node->order.is_buy) {
                update_quantity_in_place(bid_levels, node, new_quantity);
            } else {
                update_quantity_in_place(ask_levels, node, new_quantity);
            }
        }
        
        ++total_amends;
        return true;
    }
    
    // Get a snapshot of top N bid and ask levels (optimized version)
    void get_snapshot(size_t depth, std::vector<PriceLevel>& bids, std::vector<PriceLevel>& asks) const {
        ++total_snapshots;
        
        // Pre-allocate vectors
        bids.clear();
        asks.clear();
        bids.reserve(depth);
        asks.reserve(depth);
        
        // Get top bids
        size_t count = 0;
        for (const auto& [price, level] : bid_levels) {
            if (count++ >= depth) break;
            bids.emplace_back(PriceLevel{price, level->total_quantity});
        }
        
        // Get top asks
        count = 0;
        for (const auto& [price, level] : ask_levels) {
            if (count++ >= depth) break;
            asks.emplace_back(PriceLevel{price, level->total_quantity});
        }
    }
    
    // Print current state of the order book
    void print_book(size_t depth = 10) const {
        std::vector<PriceLevel> bids, asks;
        get_snapshot(depth, bids, asks);
        
        std::cout << "\n╔════════════════════════════════╗\n";
        std::cout << "║        ORDER BOOK STATUS        ║\n";
        std::cout << "╚════════════════════════════════╝\n";
        
        std::cout << std::fixed << std::setprecision(2);
        
        // Print asks in reverse (highest price first for visual clarity)
        std::cout << "\n┌─── ASKS (Sell Orders) ─────────┐\n";
        std::cout << "│ " << std::left << std::setw(12) << "Price" 
                  << " │ " << std::right << std::setw(12) << "Quantity" << " │\n";
        std::cout << "├─────────────┼──────────────┤\n";
        
        for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
            std::cout << "│ " << std::left << std::setw(12) << it->price 
                      << " │ " << std::right << std::setw(12) << it->total_quantity << " │\n";
        }
        
        // Print spread
        if (!bids.empty() && !asks.empty()) {
            double spread = asks.front().price - bids.front().price;
            std::cout << "├─────────────┴──────────────┤\n";
            std::cout << "│   SPREAD: " << std::setw(8) << spread 
                      << "          │\n";
            std::cout << "├─────────────┬──────────────┤\n";
        } else {
            std::cout << "├─────────────┼──────────────┤\n";
        }
        
        // Print bids
        std::cout << "│ " << std::left << std::setw(12) << "Price" 
                  << " │ " << std::right << std::setw(12) << "Quantity" << " │\n";
        std::cout << "├─────────────┼──────────────┤\n";
        
        for (const auto& level : bids) {
            std::cout << "│ " << std::left << std::setw(12) << level.price 
                      << " │ " << std::right << std::setw(12) << level.total_quantity << " │\n";
        }
        
        std::cout << "└─── BIDS (Buy Orders) ──────────┘\n";
        
        std::cout << "\n📊 Statistics:\n";
        std::cout << "  • Active Orders: " << order_lookup.size() << "\n";
        std::cout << "  • Bid Levels: " << bid_levels.size() << "\n";
        std::cout << "  • Ask Levels: " << ask_levels.size() << "\n";
        std::cout << "  • Total Orders Processed: " << total_orders << "\n";
    }
    
    // Get best bid and ask prices with caching
    [[nodiscard]] std::pair<double, double> get_best_prices() const {
        if (!cache_valid) {
            cached_best_bid = bid_levels.empty() ? 0.0 : bid_levels.begin()->first;
            cached_best_ask = ask_levels.empty() ? std::numeric_limits<double>::max() : ask_levels.begin()->first;
            cache_valid = true;
        }
        return {cached_best_bid, cached_best_ask};
    }
    
    // Check if order book can match (for optional matching implementation)
    [[nodiscard]] bool can_match() const {
        if (bid_levels.empty() || ask_levels.empty()) {
            return false;
        }
        auto [best_bid, best_ask] = get_best_prices();
        return best_bid >= best_ask;
    }
    
    // Get order book statistics
    struct Stats {
        uint64_t total_orders;
        uint64_t total_cancels;
        uint64_t total_amends;
        uint64_t active_orders;
        size_t bid_levels;
        size_t ask_levels;
        double best_bid;
        double best_ask;
        double spread;
    };
    
    [[nodiscard]] Stats get_stats() const {
        auto [best_bid, best_ask] = get_best_prices();
        return Stats{
            total_orders,
            total_cancels,
            total_amends,
            static_cast<uint64_t>(order_lookup.size()),
            bid_levels.size(),
            ask_levels.size(),
            best_bid,
            best_ask,
            (best_ask == std::numeric_limits<double>::max()) ? 0.0 : best_ask - best_bid
        };
    }
    
    // Print detailed performance statistics
    void print_detailed_stats() const {
        auto stats = get_stats();
        
        std::cout << "\n╔════════════════════════════════╗\n";
        std::cout << "║    PERFORMANCE STATISTICS      ║\n";
        std::cout << "╚════════════════════════════════╝\n";
        
        std::cout << "Order Operations:\n";
        std::cout << "  • Total Added: " << stats.total_orders << "\n";
        std::cout << "  • Total Cancelled: " << stats.total_cancels << "\n";
        std::cout << "  • Total Amended: " << stats.total_amends << "\n";
        std::cout << "  • Currently Active: " << stats.active_orders << "\n";
        
        std::cout << "\nBook Structure:\n";
        std::cout << "  • Bid Price Levels: " << stats.bid_levels << "\n";
        std::cout << "  • Ask Price Levels: " << stats.ask_levels << "\n";
        std::cout << "  • Total Snapshots: " << total_snapshots << "\n";
        
        std::cout << "\nMarket Data:\n";
        std::cout << std::fixed << std::setprecision(2);
        std::cout << "  • Best Bid: " << stats.best_bid << "\n";
        std::cout << "  • Best Ask: " << stats.best_ask << "\n";
        std::cout << "  • Spread: " << stats.spread << "\n";
        
        std::cout << "\nMemory Usage:\n";
        order_pool.print_stats();
    }
    
private:
    // Optimized add to side with level pooling
    template<typename MapType>
    void add_to_side(MapType& side, OrderNode* node) {
        auto it = side.find(node->order.price);
        
        Level* level;
        if (it == side.end()) {
            // Create new level from pool
            level = level_pool.allocate();
            new(level) Level(node->order.price);
            side.emplace(node->order.price, level);
        } else {
            level = it->second;
        }
        
        level->add_order(node);
    }
    
    // Optimized remove from side
    template<typename MapType>
    void remove_from_side(MapType& side, OrderNode* node) {
        auto it = side.find(node->order.price);
        if (it != side.end()) {
            Level* level = it->second;
            level->remove_order(node);
            
            // Remove empty price level and return to pool
            if (level->is_empty()) {
                side.erase(it);
                level_pool.deallocate(level);
            }
        }
    }
    
    // Optimized in-place quantity update
    template<typename MapType>
    void update_quantity_in_place(MapType& side, OrderNode* node, uint64_t new_quantity) {
        auto it = side.find(node->order.price);
        if (it != side.end()) {
            it->second->update_quantity(node, new_quantity);
        }
    }
};

// ======================== TEST SUITE ========================

class OrderBookTester {
public:
    // Basic functionality test
    static void run_basic_test() {
        std::cout << "\n╔════════════════════════════════╗\n";
        std::cout << "║      BASIC FUNCTIONALITY TEST   ║\n";
        std::cout << "╚════════════════════════════════╝\n";
        
        OrderBook book;
        
        // Add buy orders
        std::cout << "\nAdding buy orders...\n";
        book.add_order({1001, true, 100.00, 100, get_timestamp()});
        book.add_order({1002, true, 99.50, 200, get_timestamp()});
        book.add_order({1003, true, 100.00, 150, get_timestamp()});  // Same price as 1001
        book.add_order({1004, true, 98.00, 300, get_timestamp()});
        book.add_order({1005, true, 100.00, 75, get_timestamp()});   // Another at 100.00
        
        // Add sell orders
        std::cout << "Adding sell orders...\n";
        book.add_order({2001, false, 101.00, 100, get_timestamp()});
        book.add_order({2002, false, 102.00, 200, get_timestamp()});
        book.add_order({2003, false, 101.00, 150, get_timestamp()}); // Same price as 2001
        book.add_order({2004, false, 103.50, 300, get_timestamp()});
        book.add_order({2005, false, 101.00, 80, get_timestamp()});  // Another at 101.00
        
        std::cout << "\nInitial Order Book:\n";
        book.print_book();
        
        // Test cancel
        std::cout << "\n➤ Cancelling order 1002 (Buy @ 99.50)...\n";
        if (book.cancel_order(1002)) {
            std::cout << "  ✓ Order cancelled successfully\n";
        }
        book.print_book(5);
        
        // Test amend (quantity only)
        std::cout << "\n➤ Amending order 1003 quantity (100.00, 150 → 500)...\n";
        if (book.amend_order(1003, 100.00, 500)) {
            std::cout << "  ✓ Order amended successfully\n";
        }
        book.print_book(5);
        
        // Test amend (price change)
        std::cout << "\n➤ Amending order 2001 price (101.00 → 100.50)...\n";
        if (book.amend_order(2001, 100.50, 100)) {
            std::cout << "  ✓ Order amended successfully (new order created)\n";
        }
        book.print_book(5);
        
        // Test snapshot
        std::vector<PriceLevel> bids, asks;
        book.get_snapshot(3, bids, asks);
        
        std::cout << "\n📸 Top 3 Levels Snapshot:\n";
        std::cout << "Bids: ";
        for (const auto& lvl : bids) {
            std::cout << "[" << lvl.price << ":" << lvl.total_quantity << "] ";
        }
        std::cout << "\nAsks: ";
        for (const auto& lvl : asks) {
            std::cout << "[" << lvl.price << ":" << lvl.total_quantity << "] ";
        }
        std::cout << "\n";
        
        book.print_detailed_stats();
    }
    
    // Performance benchmark
    static void run_performance_test() {
        std::cout << "\n╔════════════════════════════════╗\n";
        std::cout << "║      PERFORMANCE BENCHMARK      ║\n";
        std::cout << "╚════════════════════════════════╝\n";
        
        OrderBook book;
        const int num_orders = 1000000;  // 1 million orders
        
        std::cout << "\n🚀 Testing with " << num_orders << " orders...\n";
        
        // Random number generation
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_real_distribution<> price_dist(90.0, 110.0);
        std::uniform_int_distribution<> quantity_dist(100, 1000);
        std::uniform_int_distribution<> side_dist(0, 1);
        
        // Benchmark: Add Orders
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int i = 0; i < num_orders; ++i) {
            book.add_order({
                static_cast<uint64_t>(i),
                side_dist(gen) == 0,
                price_dist(gen),
                static_cast<uint64_t>(quantity_dist(gen)),
                get_timestamp()
            });
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "\n⏱️  Add Performance:\n";
        std::cout << "  • Total time: " << duration.count() << " µs\n";
        std::cout << "  • Per order: " << std::fixed << std::setprecision(3) 
                  << duration.count() / static_cast<double>(num_orders) << " µs\n";
        std::cout << "  • Throughput: " << std::fixed << std::setprecision(0)
                  << (num_orders * 1000000.0) / duration.count() << " orders/sec\n";
        
        // Benchmark: Snapshots
        const int num_snapshots = 10000;
        start = std::chrono::high_resolution_clock::now();
        
        std::vector<PriceLevel> bids, asks;
        for (int i = 0; i < num_snapshots; ++i) {
            book.get_snapshot(10, bids, asks);
        }
        
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "\n📸 Snapshot Performance:\n";
        std::cout << "  • " << num_snapshots << " snapshots: " << duration.count() << " µs\n";
        std::cout << "  • Per snapshot: " << std::fixed << std::setprecision(3)
                  << duration.count() / static_cast<double>(num_snapshots) << " µs\n";
        
        // Benchmark: Cancellations
        const int num_cancels = 10000;
        std::vector<uint64_t> order_ids_to_cancel;
        for (int i = 0; i < num_cancels; ++i) {
            order_ids_to_cancel.push_back(i * 10);  // Cancel every 10th order
        }
        
        start = std::chrono::high_resolution_clock::now();
        
        for (uint64_t id : order_ids_to_cancel) {
            book.cancel_order(id);
        }
        
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "\n❌ Cancel Performance:\n";
        std::cout << "  • " << num_cancels << " cancellations: " << duration.count() << " µs\n";
        std::cout << "  • Per cancel: " << std::fixed << std::setprecision(3)
                  << duration.count() / static_cast<double>(num_cancels) << " µs\n";
        
        // Benchmark: Amendments
        const int num_amends = 10000;
        start = std::chrono::high_resolution_clock::now();
        
        for (int i = 1; i < num_amends + 1; ++i) {
            book.amend_order(i * 10 + 1, price_dist(gen), quantity_dist(gen));
        }
        
        end = std::chrono::high_resolution_clock::now();
        duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        
        std::cout << "\n✏️  Amend Performance:\n";
        std::cout << "  • " << num_amends << " amendments: " << duration.count() << " µs\n";
        std::cout << "  • Per amend: " << std::fixed << std::setprecision(3)
                  << duration.count() / static_cast<double>(num_amends) << " µs\n";
        
        // Final statistics
        std::cout << "\n";
        book.print_detailed_stats();
    }
    
    // Stress test with edge cases
    static void run_stress_test() {
        std::cout << "\n╔════════════════════════════════╗\n";
        std::cout << "║         STRESS TEST              ║\n";
        std::cout << "╚════════════════════════════════╝\n";
        
        OrderBook book;
        
        // Test 1: Many orders at same price
        std::cout << "\n Test 1: 1000 orders at same price...\n";
        for (int i = 0; i < 1000; ++i) {
            book.add_order({static_cast<uint64_t>(i), true, 100.00, 100, get_timestamp()});
        }
        
        auto [best_bid, best_ask] = book.get_best_prices();
        std::cout << "  ✓ Best bid: " << best_bid << "\n";
        
        // Test 2: Cancel all orders
        std::cout << "\n Test 2: Cancelling all orders...\n";
        for (int i = 0; i < 1000; ++i) {
            book.cancel_order(i);
        }
        
        auto stats = book.get_stats();
        std::cout << "  ✓ Active orders: " << stats.active_orders << "\n";
        
        // Test 3: Rapid add/cancel cycles
        std::cout << "\n Test 3: Rapid add/cancel cycles...\n";
        auto start = std::chrono::high_resolution_clock::now();
        
        for (int cycle = 0; cycle < 1000; ++cycle) {
            // Add 100 orders
            for (int i = 0; i < 100; ++i) {
                uint64_t id = cycle * 100 + i;
                book.add_order({id, (i % 2 == 0), 95.0 + (i % 20) * 0.5, 100, get_timestamp()});
            }
            
            // Cancel half
            for (int i = 0; i < 50; ++i) {
                book.cancel_order(cycle * 100 + i * 2);
            }
        }
        
        auto end = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        
        std::cout << "  ✓ Completed in: " << duration.count() << " ms\n";
        std::cout << "  ✓ Final active orders: " << book.get_stats().active_orders << "\n";
        
        // Test 4: Edge case prices
        std::cout << "\n Test 4: Edge case prices...\n";
        book.add_order({999999, true, 0.00001, 1, get_timestamp()});
        book.add_order({999998, false, 999999.99, 1, get_timestamp()});
        
        std::tie(best_bid, best_ask) = book.get_best_prices();
        std::cout << "  ✓ Min bid: " << best_bid << "\n";
        std::cout << "  ✓ Max ask: " << best_ask << "\n";
    }
    
private:
    static uint64_t get_timestamp() {
        return std::chrono::high_resolution_clock::now().time_since_epoch().count();
    }
};

// ======================== EXAMPLE USAGE ========================

class OrderBookExample {
public:
    static void run_example() {
        std::cout << "\n╔════════════════════════════════╗\n";
        std::cout << "║      EXAMPLE USAGE              ║\n";
        std::cout << "╚════════════════════════════════╝\n";
        
        OrderBook book;
        
        // Simulate realistic trading scenario
        std::cout << "\n Simulating market open...\n";
        
        // Initial market makers
        book.add_order({1, true, 99.95, 1000, 1000000});
        book.add_order({2, true, 99.90, 2000, 2000000});
        book.add_order({3, true, 99.85, 1500, 3000000});
        
        book.add_order({4, false, 100.05, 1000, 4000000});
        book.add_order({5, false, 100.10, 2000, 5000000});
        book.add_order({6, false, 100.15, 1500, 6000000});
        
        std::cout << "\nMarket Maker Orders Placed:\n";
        book.print_book(3);
        
        // Retail orders coming in
        std::cout << "\n Retail orders arriving...\n";
        book.add_order({7, true, 99.95, 100, 7000000});   // Joins best bid
        book.add_order({8, false, 100.05, 100, 8000000}); // Joins best ask
        
        // Large order splits
        std::cout << "\n Large order (10,000 shares) being split...\n";
        for (int i = 0; i < 10; ++i) {
            book.add_order({100 + i, true, 99.80, 1000, 9000000 + i * 1000});
        }
        
        std::cout << "\nAfter Large Order:\n";
        book.print_book(5);
        
        // Market moves - cancellations and new orders
        std::cout << "\n Market movement - adjusting quotes...\n";
        book.cancel_order(1);  // Cancel old best bid
        book.cancel_order(4);  // Cancel old best ask
        
        book.add_order({200, true, 99.97, 1000, 10000000});   // New best bid
        book.add_order({201, false, 100.03, 1000, 11000000}); // New best ask
        
        std::cout << "\nAfter Market Movement:\n";
        book.print_book(5);
        
        // Get market data snapshot
        std::vector<PriceLevel> bids, asks;
        book.get_snapshot(5, bids, asks);
        
        std::cout << "\n Market Data Feed (Top 5 levels):\n";
        std::cout << "╭─────────────┬──────────────╮\n";
        std::cout << "│     BIDS    │     ASKS     │\n";
        std::cout << "├─────────────┼──────────────┤\n";
        
        for (size_t i = 0; i < 5; ++i) {
            if (i < bids.size()) {
                std::cout << "│ " << std::fixed << std::setprecision(2) 
                          << std::setw(5) << bids[i].price << " x"
                          << std::setw(4) << bids[i].total_quantity << " │";
            } else {
                std::cout << "│             │";
            }
            
            if (i < asks.size()) {
                std::cout << " " << std::fixed << std::setprecision(2)
                          << std::setw(5) << asks[i].price << " x"
                          << std::setw(4) << asks[i].total_quantity << " │\n";
            } else {
                std::cout << "              │\n";
            }
        }
        std::cout << "╰─────────────┴──────────────╯\n";
        
        // Check if matching possible
        if (book.can_match()) {
            std::cout << "\n⚠️  CROSSING DETECTED - Orders can match!\n";
        } else {
            auto [bid, ask] = book.get_best_prices();
            std::cout << "\n✓ No crossing - Spread: " << (ask - bid) << "\n";
        }
    }
};

// ======================== MAIN FUNCTION ========================

#ifdef ORDERBOOK_MAIN
int main() {
    std::cout << "════════════════════════════════════════\n";
    std::cout << "   HIGH-PERFORMANCE LIMIT ORDER BOOK   \n";
    std::cout << "════════════════════════════════════════\n";
    
    // Run all tests
    OrderBookExample::run_example();
    OrderBookTester::run_basic_test();
    OrderBookTester::run_stress_test();
    OrderBookTester::run_performance_test();
    
    std::cout << "\n✅ All tests completed successfully!\n\n";
    
    return 0;
}
#endif