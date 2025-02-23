/*
 * Copyright (c) 2018 Niklas Ekström
 */

#include <exec/types.h>
#include <exec/interrupts.h>
#include <exec/lists.h>
#include <exec/memory.h>
#include <exec/nodes.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/execbase.h>

#include <libraries/dos.h>

#include <proto/exec.h>

#include <string.h>

#include "a314.h"
#include "debug.h"
#include "cmem.h"
#include "device.h"
#include "protocol.h"
#include "sockets.h"
#include "fix_mem_region.h"
#include "startup.h"

#define SysBase (*(struct ExecBase **)4)

static int used_in_r2a(struct ComArea *ca)
{
	return (ca->r2a_tail - ca->r2a_head) & 255;
}

static int used_in_a2r(struct ComArea *ca)
{
	return (ca->a2r_tail - ca->a2r_head) & 255;
}

static BOOL room_in_a2r(struct ComArea *ca, int len)
{
	return used_in_a2r(ca) + 3 + len <= 255;
}

static void append_a2r_packet(struct ComArea *ca, UBYTE type, UBYTE stream_id, UBYTE length, UBYTE *data)
{
	UBYTE index = ca->a2r_tail;
	ca->a2r_buffer[index++] = length;
	ca->a2r_buffer[index++] = type;
	ca->a2r_buffer[index++] = stream_id;
	for (int i = 0; i < (int)length; i++)
		ca->a2r_buffer[index++] = *data++;
	ca->a2r_tail = index;
}

static void close_socket(struct A314Device *dev, struct Socket *s, BOOL should_send_reset)
{
	debug_printf("Called close socket\n");

	if (s->pending_connect != NULL)
	{
		struct A314_IORequest *ior = s->pending_connect;
		ior->a314_Request.io_Error = A314_CONNECT_RESET;
		ReplyMsg((struct Message *)ior);

		s->pending_connect = NULL;
	}

	if (s->pending_read != NULL)
	{
		struct A314_IORequest *ior = s->pending_read;
		ior->a314_Length = 0;
		ior->a314_Request.io_Error = A314_READ_RESET;
		ReplyMsg((struct Message *)ior);

		s->pending_read = NULL;
	}

	if (s->pending_write != NULL)
	{
		struct A314_IORequest *ior = s->pending_write;
		ior->a314_Length = 0;
		ior->a314_Request.io_Error = A314_WRITE_RESET; // A314_EOS_RESET == A314_WRITE_RESET
		ReplyMsg((struct Message *)ior);

		s->pending_write = NULL;
	}

	if (s->rq_head != NULL)
	{
		struct QueuedData *qd = s->rq_head;
		while (qd != NULL)
		{
			struct QueuedData *next = qd->next;
			FreeMem(qd, sizeof(struct QueuedData) + qd->length);
			qd = next;
		}
		s->rq_head = NULL;
		s->rq_tail = NULL;
	}

	remove_from_send_queue(dev, s);

	// No operations can be pending when SOCKET_CLOSED is set.
	// However, may not be able to delete socket yet, because is waiting to send PKT_RESET.
	s->flags |= SOCKET_CLOSED;

	BOOL should_delete_socket = TRUE;

	if (should_send_reset)
	{
		if (dev->send_queue_head == NULL && room_in_a2r(dev->ca, 0))
		{
			append_a2r_packet(dev->ca, PKT_RESET, s->stream_id, 0, NULL);
		}
		else
		{
			s->flags |= SOCKET_SHOULD_SEND_RESET;
			add_to_send_queue(dev, s, 0);
			should_delete_socket = FALSE;
		}
	}

	if (should_delete_socket)
		delete_socket(dev, s);
}

static void handle_pkt_connect_response(struct A314Device *dev, UBYTE offset, UBYTE length, struct Socket *s)
{
	debug_printf("Received a CONNECT RESPONSE packet from rpi\n");

	if (s->pending_connect == NULL)
	{
		debug_printf("SERIOUS ERROR: received a CONNECT RESPONSE even though no connect was pending\n");
		// Should reset stream?
	}
	else if (length != 1)
	{
		debug_printf("SERIOUS ERROR: received a CONNECT RESPONSE whose length was not 1\n");
		// Should reset stream?
	}
	else
	{
		UBYTE result = dev->ca->r2a_buffer[offset];
		if (result == 0)
		{
			struct A314_IORequest *ior = s->pending_connect;
			ior->a314_Request.io_Error = A314_CONNECT_OK;
			ReplyMsg((struct Message *)ior);

			s->pending_connect = NULL;
		}
		else
		{
			struct A314_IORequest *ior = s->pending_connect;
			ior->a314_Request.io_Error = A314_CONNECT_UNKNOWN_SERVICE;
			ReplyMsg((struct Message *)ior);

			s->pending_connect = NULL;

			close_socket(dev, s, FALSE);
		}
	}
}

static void handle_pkt_data(struct A314Device *dev, UBYTE offset, UBYTE length, struct Socket *s)
{
	debug_printf("Received a DATA packet from rpi\n");

	if (s->pending_read != NULL)
	{
		struct A314_IORequest *ior = s->pending_read;

		if (ior->a314_Length < length)
			close_socket(dev, s, TRUE);
		else
		{
			UBYTE *r2a_buffer = dev->ca->r2a_buffer;
			UBYTE *dst = ior->a314_Buffer;
			for (int i = 0; i < length; i++)
				*dst++ = r2a_buffer[offset++];

			ior->a314_Length = length;
			ior->a314_Request.io_Error = A314_READ_OK;
			ReplyMsg((struct Message *)ior);

			s->pending_read = NULL;
		}
	}
	else
	{
		struct QueuedData *qd = (struct QueuedData *)AllocMem(sizeof(struct QueuedData) + length, 0);
		qd->next = NULL,
		qd->length = length;

		UBYTE *r2a_buffer = dev->ca->r2a_buffer;
		UBYTE *dst = qd->data;
		for (int i = 0; i < length; i++)
			*dst++ = r2a_buffer[offset++];

		if (s->rq_head == NULL)
			s->rq_head = qd;
		else
			s->rq_tail->next = qd;
		s->rq_tail = qd;
	}
}

static void handle_pkt_eos(struct A314Device *dev, struct Socket *s)
{
	debug_printf("Received a EOS packet from rpi\n");

	s->flags |= SOCKET_RCVD_EOS_FROM_RPI;

	if (s->pending_read != NULL)
	{
		struct A314_IORequest *ior = s->pending_read;
		ior->a314_Length = 0;
		ior->a314_Request.io_Error = A314_READ_EOS;
		ReplyMsg((struct Message *)ior);

		s->pending_read = NULL;

		s->flags |= SOCKET_SENT_EOS_TO_APP;

		if (s->flags & SOCKET_SENT_EOS_TO_RPI)
			close_socket(dev, s, FALSE);
	}
}

static void handle_r2a_packet(struct A314Device *dev, UBYTE type, UBYTE stream_id, UBYTE offset, UBYTE length)
{
	struct Socket *s = find_socket_by_stream_id(dev, stream_id);

	if (s != NULL && type == PKT_RESET)
	{
		debug_printf("Received a RESET packet from rpi\n");
		close_socket(dev, s, FALSE);
		return;
	}

	if (s == NULL || (s->flags & SOCKET_CLOSED))
	{
		// Ignore this packet. The only packet that can do anything useful on a closed
		// channel is CONNECT, which is not handled at this time.
		return;
	}

	if (type == PKT_CONNECT_RESPONSE)
	{
		handle_pkt_connect_response(dev, offset, length, s);
	}
	else if (type == PKT_DATA)
	{
		handle_pkt_data(dev, offset, length, s);
	}
	else if (type == PKT_EOS)
	{
		handle_pkt_eos(dev, s);
	}
}

static void handle_packets_received_r2a(struct A314Device *dev)
{
	struct ComArea *ca = dev->ca;

	while (used_in_r2a(ca) != 0)
	{
		UBYTE index = ca->r2a_head;

		UBYTE len = ca->r2a_buffer[index++];
		UBYTE type = ca->r2a_buffer[index++];
		UBYTE stream_id = ca->r2a_buffer[index++];

		handle_r2a_packet(dev, type, stream_id, index, len);

		index += len;
		ca->r2a_head = index;
	}
}

static void handle_room_in_a2r(struct A314Device *dev)
{
	struct ComArea *ca = dev->ca;

	while (dev->send_queue_head != NULL)
	{
		struct Socket *s = dev->send_queue_head;

		if (!room_in_a2r(ca, s->send_queue_required_length))
			break;

		remove_from_send_queue(dev, s);

		if (s->pending_connect != NULL)
		{
			struct A314_IORequest *ior = s->pending_connect;
			int len = ior->a314_Length;
			append_a2r_packet(ca, PKT_CONNECT, s->stream_id, (UBYTE)len, ior->a314_Buffer);
		}
		else if (s->pending_write != NULL)
		{
			struct A314_IORequest *ior = s->pending_write;
			int len = ior->a314_Length;

			if (ior->a314_Request.io_Command == A314_WRITE)
			{
				append_a2r_packet(ca, PKT_DATA, s->stream_id, (UBYTE)len, ior->a314_Buffer);

				ior->a314_Request.io_Error = A314_WRITE_OK;
				ReplyMsg((struct Message *)ior);

				s->pending_write = NULL;
			}
			else // A314_EOS
			{
				append_a2r_packet(ca, PKT_EOS, s->stream_id, 0, NULL);

				ior->a314_Request.io_Error = A314_EOS_OK;
				ReplyMsg((struct Message *)ior);

				s->pending_write = NULL;

				s->flags |= SOCKET_SENT_EOS_TO_RPI;

				if (s->flags & SOCKET_SENT_EOS_TO_APP)
					close_socket(dev, s, FALSE);
			}
		}
		else if (s->flags & SOCKET_SHOULD_SEND_RESET)
		{
			append_a2r_packet(ca, PKT_RESET, s->stream_id, 0, NULL);
			delete_socket(dev, s);
		}
		else
		{
			debug_printf("SERIOUS ERROR: Was in send queue but has nothing to send\n");
		}
	}
}

static void handle_app_connect(struct A314Device *dev, struct A314_IORequest *ior, struct Socket *s)
{
	debug_printf("Received a CONNECT request from application\n");

	if (s != NULL)
	{
		ior->a314_Request.io_Error = A314_CONNECT_SOCKET_IN_USE;
		ReplyMsg((struct Message *)ior);
	}
	else if (ior->a314_Length + 3 > 255)
	{
		ior->a314_Request.io_Error = A314_CONNECT_RESET;
		ReplyMsg((struct Message *)ior);
	}
	else
	{
		s = create_socket(dev, ior->a314_Request.io_Message.mn_ReplyPort->mp_SigTask, ior->a314_Socket);

		s->pending_connect = ior;
		s->flags = 0;

		int len = ior->a314_Length;
		if (dev->send_queue_head == NULL && room_in_a2r(dev->ca, len))
		{
			append_a2r_packet(dev->ca, PKT_CONNECT, s->stream_id, (UBYTE)len, ior->a314_Buffer);
		}
		else
		{
			add_to_send_queue(dev, s, len);
		}
	}
}

static void handle_app_read(struct A314Device *dev, struct A314_IORequest *ior, struct Socket *s)
{
	debug_printf("Received a READ request from application\n");

	if (s == NULL || (s->flags & SOCKET_CLOSED))
	{
		ior->a314_Length = 0;
		ior->a314_Request.io_Error = A314_READ_RESET;
		ReplyMsg((struct Message *)ior);
	}
	else
	{
		if (s->pending_connect != NULL || s->pending_read != NULL)
		{
			ior->a314_Length = 0;
			ior->a314_Request.io_Error = A314_READ_RESET;
			ReplyMsg((struct Message *)ior);

			close_socket(dev, s, TRUE);
		}
		else if (s->rq_head != NULL)
		{
			struct QueuedData *qd = s->rq_head;
			int len = qd->length;

			if (ior->a314_Length < len)
			{
				ior->a314_Length = 0;
				ior->a314_Request.io_Error = A314_READ_RESET;
				ReplyMsg((struct Message *)ior);

				close_socket(dev, s, TRUE);
			}
			else
			{
				s->rq_head = qd->next;
				if (s->rq_head == NULL)
					s->rq_tail = NULL;

				memcpy(ior->a314_Buffer, qd->data, len);
				FreeMem(qd, sizeof(struct QueuedData) + len);

				ior->a314_Length = len;
				ior->a314_Request.io_Error = A314_READ_OK;
				ReplyMsg((struct Message *)ior);
			}
		}
		else if (s->flags & SOCKET_RCVD_EOS_FROM_RPI)
		{
			ior->a314_Length = 0;
			ior->a314_Request.io_Error = A314_READ_EOS;
			ReplyMsg((struct Message *)ior);

			s->flags |= SOCKET_SENT_EOS_TO_APP;

			if (s->flags & SOCKET_SENT_EOS_TO_RPI)
				close_socket(dev, s, FALSE);
		}
		else
			s->pending_read = ior;
	}
}

static void handle_app_write(struct A314Device *dev, struct A314_IORequest *ior, struct Socket *s)
{
	debug_printf("Received a WRITE request from application\n");

	if (s == NULL || (s->flags & SOCKET_CLOSED))
	{
		ior->a314_Length = 0;
		ior->a314_Request.io_Error = A314_WRITE_RESET;
		ReplyMsg((struct Message *)ior);
	}
	else
	{
		int len = ior->a314_Length;
		if (s->pending_connect != NULL || s->pending_write != NULL || (s->flags & SOCKET_RCVD_EOS_FROM_APP) || len + 3 > 255)
		{
			ior->a314_Length = 0;
			ior->a314_Request.io_Error = A314_WRITE_RESET;
			ReplyMsg((struct Message *)ior);

			close_socket(dev, s, TRUE);
		}
		else
		{
			if (dev->send_queue_head == NULL && room_in_a2r(dev->ca, len))
			{
				append_a2r_packet(dev->ca, PKT_DATA, s->stream_id, (UBYTE)len, ior->a314_Buffer);

				ior->a314_Request.io_Error = A314_WRITE_OK;
				ReplyMsg((struct Message *)ior);
			}
			else
			{
				s->pending_write = ior;
				add_to_send_queue(dev, s, len);
			}
		}
	}
}

static void handle_app_eos(struct A314Device *dev, struct A314_IORequest *ior, struct Socket *s)
{
	debug_printf("Received an EOS request from application\n");

	if (s == NULL || (s->flags & SOCKET_CLOSED))
	{
		ior->a314_Request.io_Error = A314_EOS_RESET;
		ReplyMsg((struct Message *)ior);
	}
	else
	{
		if (s->pending_connect != NULL || s->pending_write != NULL || (s->flags & SOCKET_RCVD_EOS_FROM_APP))
		{
			ior->a314_Length = 0;
			ior->a314_Request.io_Error = A314_EOS_RESET;
			ReplyMsg((struct Message *)ior);

			close_socket(dev, s, TRUE);
		}
		else
		{
			s->flags |= SOCKET_RCVD_EOS_FROM_APP;

			if (dev->send_queue_head == NULL && room_in_a2r(dev->ca, 0))
			{
				append_a2r_packet(dev->ca, PKT_EOS, s->stream_id, 0, NULL);

				ior->a314_Request.io_Error = A314_EOS_OK;
				ReplyMsg((struct Message *)ior);

				s->flags |= SOCKET_SENT_EOS_TO_RPI;

				if (s->flags & SOCKET_SENT_EOS_TO_APP)
					close_socket(dev, s, FALSE);
			}
			else
			{
				s->pending_write = ior;
				add_to_send_queue(dev, s, 0);
			}
		}
	}
}

static void handle_app_reset(struct A314Device *dev, struct A314_IORequest *ior, struct Socket *s)
{
	debug_printf("Received a RESET request from application\n");

	if (s == NULL || (s->flags & SOCKET_CLOSED))
	{
		ior->a314_Request.io_Error = A314_RESET_OK;
		ReplyMsg((struct Message *)ior);
	}
	else
	{
		ior->a314_Request.io_Error = A314_RESET_OK;
		ReplyMsg((struct Message *)ior);

		close_socket(dev, s, TRUE);
	}
}

static void handle_app_request(struct A314Device *dev, struct A314_IORequest *ior)
{
	struct Socket *s = find_socket(dev, ior->a314_Request.io_Message.mn_ReplyPort->mp_SigTask, ior->a314_Socket);

	switch (ior->a314_Request.io_Command)
	{
	case A314_CONNECT:
		handle_app_connect(dev, ior, s);
		break;
	case A314_READ:
		handle_app_read(dev, ior, s);
		break;
	case A314_WRITE:
		handle_app_write(dev, ior, s);
		break;
	case A314_EOS:
		handle_app_eos(dev, ior, s);
		break;
	case A314_RESET:
		handle_app_reset(dev, ior, s);
		break;
	default:
		ior->a314_Request.io_Error = IOERR_NOCMD;
		ReplyMsg((struct Message *)ior);
		break;
	}
}

void task_main()
{
	struct A314Device *dev = (struct A314Device *)FindTask(NULL)->tc_UserData;
	struct ComArea *ca = dev->ca;

	while (TRUE)
	{
		debug_printf("Waiting for signal\n");

		ULONG signal = Wait(SIGF_MSGPORT | SIGF_INT);

		UBYTE prev_a2r_tail = ca->a2r_tail;
		UBYTE prev_r2a_head = ca->r2a_head;

		if (signal & SIGF_MSGPORT)
		{
			write_cmem_safe(A_ENABLE_ADDRESS, 0);

			struct Message *msg;
			while (msg = GetMsg(&dev->task_mp))
				handle_app_request(dev, (struct A314_IORequest *)msg);
		}

		UBYTE a_enable = 0;
		while (a_enable == 0)
		{
			handle_packets_received_r2a(dev);
			handle_room_in_a2r(dev);

			UBYTE r_events = 0;
			if (ca->a2r_tail != prev_a2r_tail)
				r_events |= R_EVENT_A2R_TAIL;
			if (ca->r2a_head != prev_r2a_head)
				r_events |= R_EVENT_R2A_HEAD;

			Disable();
			UBYTE prev_regd = read_cp_nibble(13);
			write_cp_nibble(13, prev_regd | 8);
			read_cp_nibble(A_EVENTS_ADDRESS);

			if (ca->r2a_head == ca->r2a_tail)
			{
				if (dev->send_queue_head == NULL)
					a_enable = A_EVENT_R2A_TAIL;
				else if (!room_in_a2r(ca, dev->send_queue_head->send_queue_required_length))
					a_enable = A_EVENT_R2A_TAIL | A_EVENT_A2R_HEAD;

				if (a_enable != 0)
				{
					write_cp_nibble(A_ENABLE_ADDRESS, a_enable);
					if (r_events != 0)
						write_cp_nibble(R_EVENTS_ADDRESS, r_events);
				}
			}

			write_cp_nibble(13, prev_regd);
			Enable();
		}
	}

	// There is currently no way to unload a314.device.

	//debug_printf("Shutting down\n");

	//RemIntServer(INTB_PORTS, &ports_interrupt);
	//RemIntServer(INTB_VERTB, &vertb_interrupt);
	//FreeMem(ca, sizeof(struct ComArea));

	// Stack and task structure should be reclaimed.
}
