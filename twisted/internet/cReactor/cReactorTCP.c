/*
 * Twisted, the Framework of Your Internet
 * Copyright (C) 2001-2002 Matthew W. Lefkowitz
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of version 2.1 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 * 
 */
/* cReactorTCP.c - Implementation of IReactorTCP. */

/* includes */
#include "cReactor.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

/* Forward declare the type object. */
staticforward PyTypeObject cReactorListeningPortType;

/* The (temporary) IListeningPort object. */
typedef struct 
{
    PyObject_HEAD

    /* The reactor we are part of. */
    PyObject *          reactor;

    /* Our socket file descriptor. */
    int                 fd;

    /* The port we are listening on (for repr). */
    int                 port;
} cReactorListeningPort;


/* Called when there is data to read. */
static void
tcp_do_read(cReactorTransport *transport)
{
    char buffer[1024];
    int bytes_in;
    PyObject *py_buf;
    PyObject *result;

    /* Attempt to read. */
    bytes_in = recv(transport->fd, buffer, sizeof(buffer), 0);
    if (bytes_in < 0)
    {
        perror("recv");
    }
    else if (bytes_in == 0)
    {
        /* The connection is gone. */
        result = PyObject_CallMethod(transport->object, "connectionLost", NULL);
        Py_XDECREF(result);
        if (!result)
        {
            PyErr_Print();
        }

        /* Close this transport and tell the reactor that the FD list is stale.
         */
        transport->state                    = CREACTOR_TRANSPORT_STATE_CLOSED;
        transport->reactor->pollfd_stale    = 1;
    }
    else if (bytes_in > 0)
    {
        /* Make a Python string. */
        py_buf = PyString_FromStringAndSize(buffer, bytes_in);

        /* Give the data to the protocol. */
        result = PyObject_CallMethod(transport->object, "dataReceived", "(O)", py_buf);
        Py_DECREF(py_buf);
        Py_XDECREF(result);
        if (!result)
        {
            PyErr_Print();
        }
    }
}

/* Called when writing will not block. */
static void
tcp_do_write(cReactorTransport *transport)
{
    unsigned int avail;
    int bytes_out;

    /* Determine how many bytes we have to write. */
    avail = cReactorBuffer_DataAvailable(transport->out_buf);
    if (avail > 0)
    {
        /* Attempt to send. */
        bytes_out = send(transport->fd,
                         cReactorBuffer_GetPtr(transport->out_buf),
                         avail,
                         0);

        if (bytes_out <= 0)
        {
            perror("send");
            return;
        }
        else
        {
            cReactorBuffer_Seek(transport->out_buf, bytes_out);
            avail = cReactorBuffer_DataAvailable(transport->out_buf);
        }
    }

    /* Check for end-of-buffer. */
    if (avail == 0)
    {
        /* Remove the POLLOUT event. */
        *transport->event_mask = (*transport->event_mask) & (~POLLOUT);

        /* If we are in the CLOSING state, move us to CLOSED. */
        if (transport->state == CREACTOR_TRANSPORT_STATE_CLOSING)
        {
            transport->state = CREACTOR_TRANSPORT_STATE_CLOSED;
            transport->reactor->pollfd_stale = 1;
        }
    }
}

static void
tcp_do_close(cReactorTransport *transport)
{
    PyObject *result;

    close(transport->fd);
    transport->fd = -1;

    /* Call "connectionLost" on our protocol. */
    result = PyObject_CallMethod(transport->object, "connectionLost", NULL);
    Py_XDECREF(result);
    if (!result)
    {
        PyErr_Print();
    }

    Py_DECREF(transport->object);
    transport->object = NULL;
}

static PyObject *
tcp_get_host(cReactorTransport *transport)
{
    struct sockaddr_in addr;
    int addr_len;

    addr_len = sizeof(addr);
    if (getsockname(transport->fd, (struct sockaddr *)&addr, &addr_len) < 0)
    {
        PyErr_SetFromErrno(PyExc_RuntimeError);
        return NULL;
    }

    return Py_BuildValue("(ssi)", "INET", inet_ntoa(addr.sin_addr),
                         ntohs(addr.sin_port));
}


static PyObject *
tcp_get_peer(cReactorTransport *transport)
{
    struct sockaddr_in addr;
    int addr_len;

    addr_len = sizeof(addr);
    if (getpeername(transport->fd, (struct sockaddr *)&addr, &addr_len) < 0)
    {
        PyErr_SetFromErrno(PyExc_RuntimeError);
        return NULL;
    }

    return Py_BuildValue("(ssi)", "INET", inet_ntoa(addr.sin_addr),
                         ntohs(addr.sin_port));
}

/* Implementation of a transport 'do_read' function for a TCP listening
 * socket.  Called when there is data to read.
 */
static void
tcp_listen_do_read(cReactorTransport *transport)
{
    int new_fd;
    struct sockaddr_in addr;
    int addr_len;
    PyObject *protocol;
    cReactorTransport *proto_trans;
    PyObject *result;

    /* Try to accept(). */
    addr_len = sizeof(struct sockaddr_in);
    new_fd = accept(transport->fd, (struct sockaddr *)&addr, &addr_len);

    /* Bail out on accept failures. */
    if (new_fd < 0)
    {
        /* TODO: check errors to see if there is anything we should do. */
        return;
    }

    /* Create a new protocol instance from the factory. */
    protocol = PyObject_CallMethod(transport->object,
                                   "buildProtocol",
                                   "(s)",
                                   "internet-address-here");
    if (!protocol)
    {
        PyErr_Print();
        close(new_fd);
        return;
    }

    /* Make a new transport for this protocol.  The transport now own the
     * newly created protocol.
     */
    proto_trans = cReactorTransport_New(transport->reactor,
                                         new_fd,
                                         tcp_do_read,
                                         tcp_do_write,
                                         tcp_do_close);
    proto_trans->get_peer   = tcp_get_peer;
    proto_trans->get_host   = tcp_get_host;
    proto_trans->object     = protocol;

    /* Connect them together. */
    result = PyObject_CallMethod(protocol,
                                 "makeConnection",
                                 "(O)",
                                 proto_trans);
    Py_XDECREF(result);
    if (!result)
    {
        PyErr_Print();
        Py_DECREF(proto_trans);
        return;
    }

    /* Add the new transport into the reactor. */
    cReactor_AddTransport(transport->reactor, proto_trans);
}

static void
tcp_listen_do_close(cReactorTransport *transport)
{
    close(transport->fd);
    transport->fd = -1;

    Py_DECREF(transport->object);
    transport->object = NULL;
}


PyObject *
cReactorTCP_listenTCP(PyObject *self, PyObject *args, PyObject *kw)
{
    int port;
    PyObject *factory;
    int backlog             = 5;
    const char *interface   = "";
    int sock;
    struct sockaddr_in addr;
    int opt;
    cReactorListeningPort *port_obj;
    PyObject *result;
    cReactorTransport *transport;
    cReactor *reactor;
    static char *kwlist[] = { "port", "factory", "backlog", "interface", NULL };

    reactor = (cReactor *)self;

    /* Args. */
    if (!PyArg_ParseTupleAndKeywords(args, kw, "iO|is:listenTCP", kwlist,
                                     &port, &factory, &backlog, &interface))
    {
        return NULL;
    }

    /*
    printf("listenTCP: %d ", port);
    PyObject_Print(factory, stdout, 1);
    printf(" %d \"%s\"\n", backlog, interface);
    */

    /* Tell the factory to start. */
    result = PyObject_CallMethod(factory, "doStart", NULL);
    Py_XDECREF(result);
    if (!result)
    {
        return NULL;
    }

    /* Make the TCP socket. */
    sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0)
    {
        return PyErr_SetFromErrno(PyExc_RuntimeError);
    }

    /* Non-blocking. */
    if (fcntl(sock, F_SETFL, O_NONBLOCK) < 0)
    {
        close(sock);
        return PyErr_SetFromErrno(PyExc_RuntimeError);
    }

    /* Enable reuse address. */
    opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        close(sock);
        return PyErr_SetFromErrno(PyExc_RuntimeError);
    }

    /* Form the address. */
    addr.sin_family         = AF_INET;
    addr.sin_port           = htons(port);
    addr.sin_addr.s_addr    = htonl(INADDR_ANY);
    
    /* Bind. */
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(sock);
        return PyErr_SetFromErrno(PyExc_RuntimeError);
    }

    /* Enable listening. */
    if (listen(sock, backlog) < 0)
    {
        close(sock);
        return PyErr_SetFromErrno(PyExc_RuntimeError);
    }

    /* Create a read-only transport. */
    transport = cReactorTransport_New(reactor,
                                       sock,
                                       tcp_listen_do_read,
                                       NULL,
                                       tcp_listen_do_close);
    Py_INCREF(factory);
    transport->object = factory;

    cReactor_AddTransport(reactor, transport);

    /* TEMP: Create the ListeningPort object. */
    port_obj = PyObject_New(cReactorListeningPort,
                            &cReactorListeningPortType);
    Py_INCREF(self);
    port_obj->reactor   = self;
    port_obj->fd        = sock;
    port_obj->port      = port;

    return (PyObject *)port_obj;
}



PyObject *
cReactorTCP_clientTCP(PyObject *self, PyObject *args)
{
    return cReactor_not_implemented(self, args, "cReactor_clientTCP");
}


/*
 * The following code is temporary, and can be removed if the listenTCP
 * interface changes to return an ID instead of an object.
 */

static PyObject *
cReactorListeningPort_stopListening(PyObject *self, PyObject *args)
{
    return cReactor_not_implemented(self, args, "cReactorListeningPort_stopListening");
}


static void
cReactorListeningPort_dealloc(PyObject *self)
{
    cReactorListeningPort *port;

    /* Nothing to do in the destructor. */
    port = (cReactorListeningPort *)self;
    Py_DECREF(port->reactor);
    PyObject_Del(self);
}

static PyMethodDef cReactorListeningPort_methods[] = 
{
    { "stopListening", cReactorListeningPort_stopListening,
      METH_VARARGS, "stopListening" },
    { NULL, NULL, METH_VARARGS, NULL },
};

static PyObject *
cReactorListeningPort_getattr(PyObject *self, char *name)
{
    return Py_FindMethod(cReactorListeningPort_methods, self, name);
}

static PyObject *
cReactorListeningPort_repr(PyObject *self)
{
    char buf[100];
    cReactorListeningPort *port;

    port = (cReactorListeningPort *)self;

    snprintf(buf, sizeof(buf) - 1, "<cReactorListeningPort port=%d>",
             port->port);
    buf[sizeof(buf) - 1] = 0x00;
    
    return PyString_FromString(buf);
}

/* The ListeningPort type. */
static PyTypeObject cReactorListeningPortType = 
{
    PyObject_HEAD_INIT(&PyType_Type)
    0,
    "cReactorListeningPort", /* tp_name */
    sizeof(cReactorListeningPort),  /* tp_basicsize */
    0,                  /* tp_itemsize */
    cReactorListeningPort_dealloc,   /* tp_dealloc */
    NULL,               /* tp_print */
    cReactorListeningPort_getattr,   /* tp_getattr */
    NULL,               /* tp_setattr */
    NULL,               /* tp_compare */
    cReactorListeningPort_repr,      /* tp_repr */
    NULL,               /* tp_as_number */
    NULL,               /* tp_as_sequence */
    NULL,               /* tp_as_mapping */
    NULL,               /* tp_hash */
    NULL,               /* tp_call */
    NULL,               /* tp_str */
    NULL,               /* tp_getattro */
    NULL,               /* tp_setattro */
    NULL,               /* tp_as_buffer */
    0,                  /* tp_flags */
    NULL,               /* tp_doc */
    NULL,               /* tp_traverse */
    NULL,               /* tp_clear */
    NULL,               /* tp_richcompare */
    0,                  /* tp_weaklistoffset */
};

/* vim: set sts=4 sw=4: */
