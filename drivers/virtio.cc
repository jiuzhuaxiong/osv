/*
 * Copyright (C) 2013 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <string.h>

#include "drivers/virtio.hh"
#include "virtio-vring.hh"
#include <osv/debug.h>
#include "osv/trace.hh"

TRACEPOINT(trace_virtio_wait_for_queue, "queue(%p) have_elements=%d", void*, int);

namespace virtio {

int virtio_driver::_disk_idx = 0;

virtio_driver::virtio_driver(virtio_device& dev)
    : hw_driver()
    , _dev(dev)
    , _num_queues(0)
    , _cap_indirect_buf(false)
{
    for (unsigned i = 0; i < max_virtqueues_nr; i++) {
        _queues[i] = nullptr;
    }

    // Initialize device
    _dev.init();

    // Step 1 - make sure the device is reset
    reset_device();

    // Steps 2 & 3 - acknowledge device
    add_dev_status(VIRTIO_CONFIG_S_ACKNOWLEDGE);
    add_dev_status(VIRTIO_CONFIG_S_DRIVER);
}

virtio_driver::~virtio_driver()
{
    reset_device();
    free_queues();
}

void virtio_driver::setup_features()
{
    // Step 4 - negotiate features
    u64 dev_features = get_device_features();
    u64 drv_features = this->get_driver_features();

    u64 subset = dev_features & drv_features;

    //notify the host about the features in used according
    //to the virtio spec
    for (int i = 0; i < 64; i++)
        if (subset & (1 << i))
            virtio_d("%s: found feature intersec of bit %d", __FUNCTION__,  i);

    if (subset & (1 << VIRTIO_RING_F_INDIRECT_DESC))
        set_indirect_buf_cap(true);

    if (subset & (1 << VIRTIO_RING_F_EVENT_IDX))
            set_event_idx_cap(true);

    set_guest_features(subset);

    // Step 5 - confirm features (only applies to modern devices)
    if (_dev.is_modern())
        add_dev_status(VIRTIO_CONFIG_S_FEATURES_OK);
}

void virtio_driver::dump_config()
{
    _dev.dump_config();

    virtio_d("    virtio features: ");
    for (int i = 0; i < 64; i++)
        virtio_d(" %d ", get_device_feature_bit(i));
}

void virtio_driver::reset_device()
{
    set_dev_status(0);
}

void virtio_driver::free_queues()
{
    for (unsigned i = 0; i < max_virtqueues_nr; i++) {
        if (nullptr != _queues[i]) {
            delete (_queues[i]);
            _queues[i] = nullptr;
        }
    }
}

bool virtio_driver::kick(int queue)
{
    _dev.kick_queue(queue);
    return true;
}

void virtio_driver::probe_virt_queues()
{
    u16 qsize = 0;

    do {

        if (_num_queues >= max_virtqueues_nr) {
            return;
        }

        // Read queue size
        _dev.select_queue(_num_queues);
        qsize = _dev.get_queue_size();
        if (0 == qsize) {
            break;
        }

        // Init a new queue
        vring* queue = new vring(this, qsize, _num_queues);
        _queues[_num_queues] = queue;

        // Activate queue
        _dev.setup_queue(queue);
        _num_queues++;

        // Debug print
        virtio_d("Queue[%d] -> size %d, paddr %x", (_num_queues-1), qsize, queue->get_paddr());

    } while (true);

    for (u32 _q = 0; _q < _num_queues; _q++)
        _dev.activate_queue(_q);
}

vring* virtio_driver::get_virt_queue(unsigned idx)
{
    if (idx >= _num_queues) {
        return nullptr;
    }

    return _queues[idx];
}

void virtio_driver::wait_for_queue(vring* queue, bool (vring::*pred)() const)
{
    sched::thread::wait_until([queue,pred] {
        bool have_elements = (queue->*pred)();
        if (!have_elements) {
            queue->enable_interrupts();

            // we must check that the ring is not empty *after*
            // we enable interrupts to avoid a race where a packet
            // may have been delivered between queue->used_ring_not_empty()
            // and queue->enable_interrupts() above
            have_elements = (queue->*pred)();
            if (have_elements) {
                queue->disable_interrupts();
            }
        }

        trace_virtio_wait_for_queue(queue, have_elements);
        return have_elements;
    });
}

u64 virtio_driver::get_device_features()
{
    return _dev.get_available_features();
}

bool virtio_driver::get_device_feature_bit(int bit)
{
    return _dev.get_available_feature_bit(bit);
}

void virtio_driver::set_guest_features(u64 features)
{
    _dev.set_enabled_features(features);
}

bool virtio_driver::get_guest_feature_bit(int bit)
{
    return _dev.get_enabled_feature_bit(bit);
}

u8 virtio_driver::get_dev_status()
{
    return _dev.get_status();
}

void virtio_driver::set_dev_status(u8 status)
{
    _dev.set_status(status);
}

void virtio_driver::add_dev_status(u8 status)
{
    _dev.set_status(get_dev_status() | status);
}

void virtio_driver::virtio_conf_read(u32 offset, void* buf, int length)
{
    unsigned char* ptr = reinterpret_cast<unsigned char*>(buf);
    for (int i = 0; i < length; i++)
        ptr[i] = _dev.read_config(offset + i);
}

}
