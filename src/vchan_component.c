/*
 * Copyright 2014, NICTA
 *
 * This software may be distributed and modified according to the terms of
 * the GNU General Public License version 2. Note that NO WARRANTY is provided.
 * See "LICENSE_GPLv2.txt" for details.
 *
 * @TAG(NICTA_GPL)
 */

#define __NEED_off_t

#include <sel4vchan/vmm_manager.h>
#include <sel4vchan/vchan_component.h>
#include <sel4vchan/vchan_sharemem.h>

static libvchan_t *vchan_init(int domain, int port, int server);
static int libvchan_readwrite_action(libvchan_t *ctrl, void *data, size_t size, int stream, int action);

/*
    Set up the vchan connection interface
    Currently, the number of vchan connection interfaces allowed is hardcoded to 1 per component
*/
libvchan_t *link_vchan_comp(libvchan_t *ctrl, camkes_vchan_con_t *vchan_com) {
    if(ctrl == NULL)
        return NULL;

    int res;
    ctrl->con = vchan_com;
    /* Perform vchan component initialisation */
    vchan_connect_t t = {
        .v.domain = vchan_com->source_dom_number,
        .v.dest = ctrl->domain_num,
        .v.port = ctrl->port_num,
        .server = ctrl->is_server,
    };

    res = ctrl->con->connect(t);
    if(res < 0) {
        free(ctrl);
        return NULL;
    }
    return ctrl;
}

/*
    Create a new vchan server instance
*/
libvchan_t *libvchan_server_init(int domain, int port, size_t read_min, size_t write_min) {
    return vchan_init(domain, port, 1);
}

/*
    Create a new vchan client instance
*/
libvchan_t *libvchan_client_init(int domain, int port) {
    return vchan_init(domain, port, 0);
}

/*
    Create a new client/server instance
*/
libvchan_t *vchan_init(int domain, int port, int server) {
    libvchan_t *new_connection = malloc(sizeof(libvchan_t));
    if(new_connection == NULL) {
        return NULL;
    }

    new_connection->is_server = server;
    new_connection->server_persists = 1;
    new_connection->blocking = 1;
    new_connection->domain_num = domain;
    new_connection->port_num = port;

    return new_connection;
}

/*
    Reading and writing to the vchan
*/
int libvchan_write(libvchan_t *ctrl, const void *data, size_t size) {
    return libvchan_readwrite_action(ctrl, (void *) data, size, 1, VCHAN_SEND);
}

int libvchan_send(libvchan_t *ctrl, const void *data, size_t size) {
    return libvchan_readwrite_action(ctrl, (void *) data, size, 0, VCHAN_SEND);
}

int libvchan_read(libvchan_t *ctrl, void *data, size_t size) {
    return libvchan_readwrite_action(ctrl, data, size, 1, VCHAN_RECV);
}

int libvchan_recv(libvchan_t *ctrl, void *data, size_t size) {
    return libvchan_readwrite_action(ctrl, data, size, 0, VCHAN_RECV);
}

/*
    Return correct buffer for given vchan read/write action
*/
vchan_buf_t *get_vchan_buf(vchan_ctrl_t *args, camkes_vchan_con_t *c, int action) {
    intptr_t buf_pos = c->get_buf(*args, action);
    /* Check that a buffer was retrieved */
    if(buf_pos == 0) {
        return NULL;
    }

    if(c->data_buf == NULL) {
        return NULL;
    }

    void *addr = c->data_buf;
    addr += buf_pos;
    vchan_buf_t *b = (vchan_buf_t *) (addr);

    return b;
}

/*
    Helper function
    Allows connected components to get correct vchan buffer for read/write
*/
vchan_buf_t *get_vchan_ctrl_databuf(libvchan_t *ctrl, int action) {
    vchan_ctrl_t args = {
        .domain = ctrl->con->source_dom_number,
        .dest = ctrl->domain_num,
        .port = ctrl->port_num,
    };

    return get_vchan_buf(&args, ctrl->con, action);
}

/*
    Perform a vchan read/write action into a given buffer
     This function is intended for non Init components, Init components have a different method
*/
int libvchan_readwrite_action(libvchan_t *ctrl, void *data, size_t size, int stream, int action) {
    int *update;
    vchan_buf_t *b = get_vchan_ctrl_databuf(ctrl, action);
    if(b == NULL) {
        return -1;
    }

    /*
        How data is stored in a given vchan buffer

        Position of data in buffer is given by
            (either b->write_pos or b->read_pos) % VCHAN_BUF_SIZE
            read_pos is incremented by x when x bytes are read from the buffer
            write_pos is incremented by x when x bytes are read from the buffer

        Amount of bytes in buffer is given by the difference between read_pos and write_pos
            if write_pos > read_pos, there is data yet to be read
    */
    size_t filled = abs(b->write_pos - b->read_pos);
    if(action == VCHAN_SEND) {
        if(stream) {
            size_t ssize = MIN(VCHAN_BUF_SIZE - filled, size);
            while(ssize == 0) {
                ctrl->con->wait();
                filled = abs(b->write_pos - b->read_pos);
                ssize = MIN(VCHAN_BUF_SIZE - filled, size);
            }
            size = ssize;
        } else {
            while(size > (VCHAN_BUF_SIZE - filled)) {
                ctrl->con->wait();
                filled = abs(b->write_pos - b->read_pos);
            }
        }
        update = &b->write_pos;
    } else {
        if(stream) {
            size_t ssize = MIN(filled, size);
            while(ssize == 0) {
                ctrl->con->wait();
                filled = abs(b->write_pos - b->read_pos);
                ssize = MIN(filled, size);
            }
            size = ssize;
        } else {
            while(size > filled) {
                ctrl->con->wait();
                filled = abs(b->write_pos - b->read_pos);
            }
        }
        update = &b->read_pos;
    }

    /*
        Because this buffer is circular,
            data may have to wrap around to the start of the buffer
            This is achieved by doing two copies, one to buffer end
            And one at start of buffer for remaining data

        E.g if buffer size = 12
        and if write pos = 7 && number of bytes to write = 8

        Start:
                write_pos
                    V
            [oooooooooooo]
        End:
            write_pos
                V
            [xxxooooxxxxx]

    */
    off_t start = (*update % VCHAN_BUF_SIZE);
    off_t remain = 0;

    if(start + size > VCHAN_BUF_SIZE) {
        remain = (start + size) - VCHAN_BUF_SIZE;
        size -= remain;
    }

    void *dbuf = &b->sync_data;

    if(action == VCHAN_SEND) {
        /*
            src = address given by function caller
            dest = vchan dataport
        */
        memcpy(dbuf + start, data, size);
        memcpy(dbuf, data + size, remain);
    } else {
        /*
            src = vchan dataport
            dest = address given by function caller
        */
        memcpy(data, ((void *) dbuf) + start, size);
        memcpy(data + size, dbuf, remain);
    }
    __sync_synchronize();

    /*
        Update either the read byte counter or the written byte counter
            With how much was written or read
    */
    *update += (size + remain);

    ctrl->con->alert();

    return (size + remain);
}


/*
    Wait for data to arrive to a component from a given vchan
*/
int libvchan_wait(libvchan_t *ctrl) {
    vchan_buf_t *b = get_vchan_ctrl_databuf(ctrl, VCHAN_RECV);
    assert(b != NULL);
    size_t filled = abs(b->write_pos - b->read_pos);
    while(filled == 0) {
        ctrl->con->wait();
        b = get_vchan_ctrl_databuf(ctrl, VCHAN_RECV);
        filled = abs(b->write_pos - b->read_pos);
    }

    return 0;
}

void libvchan_close(libvchan_t *ctrl) {
    vchan_connect_t t = {
        .v.domain = ctrl->con->source_dom_number,
        .v.dest = ctrl->domain_num,
        .v.port = ctrl->port_num,
        .server = ctrl->is_server,
    };

    ctrl->con->disconnect(t);
}

int libvchan_is_open(libvchan_t *ctrl) {
    vchan_ctrl_t args = {
        .domain = ctrl->con->source_dom_number,
        .dest = ctrl->domain_num,
        .port = ctrl->port_num,
    };

    return ctrl->con->status(args);
}

/* Returns nonzero if the peer has closed connection */
int libvchan_is_eof(libvchan_t *ctrl) {
    if(libvchan_is_open(ctrl) != 1) {
        return 1;
    }
    return 0;
}

/*
    How much data can be read from the vchan
*/
int libvchan_data_ready(libvchan_t *ctrl) {
    vchan_buf_t *b = get_vchan_ctrl_databuf(ctrl, VCHAN_RECV);
    assert(b != NULL);
    size_t filled = abs(b->write_pos - b->read_pos);

    return filled;
}

/*
    How much data can be written to the vchan
*/
int libvchan_buffer_space(libvchan_t *ctrl) {
    if(libvchan_is_open(ctrl) != 1) {
        return 0;
    }

    vchan_buf_t *b = get_vchan_ctrl_databuf(ctrl, VCHAN_SEND);
    assert(b != NULL);
    size_t filled = abs(b->write_pos - b->read_pos);

    return VCHAN_BUF_SIZE - filled;
}
