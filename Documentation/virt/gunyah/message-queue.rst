.. SPDX-License-Identifier: GPL-2.0

Message Queues
==============
Message queue is a simple low-capacity IPC channel between two virtual machines.
It is intended for sending small control and configuration messages. Each
message queue is unidirectional, so a full-duplex IPC channel requires a pair of
queues.

Messages can be up to 240 bytes in length. Longer messages require a further
protocol on top of the message queue messages themselves. For instance,
communication with the resource manager adds a header field for sending longer
messages via multiple message fragments.

The diagram below shows how message queue works. A typical configuration
involves 2 message queues. Message queue 1 allows VM_A to send messages to VM_B.
Message queue 2 allows VM_B to send messages to VM_A.

1. VM_A sends a message of up to 240 bytes in length. It raises a hypercall
   with the message to inform the hypervisor to add the message to
   message queue 1's queue. The hypervisor copies memory into the internal
   message queue representation; the memory doesn't need to be shared between
   VM_A and VM_B.

2. Gunyah raises the corresponding interrupt for VM_B (Rx vIRQ) when any of
   these happens:

   a. gunyah_msgq_send() has PUSH flag. This is a typical case.
   b. Explicility with gunyah_msgq_push command from VM_A.
   c. Message queue has reached a threshold depth. Typically, this threshold
      depth is the size of the queue (in other words: when queue is full, Rx
      vIRQ raised).

3. VM_B calls gunyah_msgq_recv() and Gunyah copies message to requested buffer.

4. Gunyah raises the corresponding interrupt for VM_A (Tx vIRQ) when the message
   queue falls below a watermark depth. Typically, this is the size of the queue
   (in other words: when the queue is no longer full, Tx vIRQ raised). Note the
   watermark depth and the threshold depth for the Rx vIRQ are independent
   values, although they are both typically the size of the queue.
   Coincidentally, this signal is conceptually similar to Clear-to-Send.

For VM_B to send a message to VM_A, the process is identical, except that
hypercalls reference message queue 2's capability ID. Each message queue has its
own independent vIRQ: two TX message queues will have two vIRQs (and two
capability IDs).

::

      +---------------+         +-----------------+         +---------------+
      |      VM_A     |         |Gunyah hypervisor|         |      VM_B     |
      |               |         |                 |         |               |
      |               |         |                 |         |               |
      |               |   Tx    |                 |         |               |
      |               |-------->|                 | Rx vIRQ |               |
      |gunyah_msgq_send() | Tx vIRQ |Message queue 1  |-------->|gunyah_msgq_recv() |
      |               |<------- |                 |         |               |
      |               |         |                 |         |               |
      | Message Queue |         |                 |         | Message Queue |
      | driver        |         |                 |         | driver        |
      |               |         |                 |         |               |
      |               |         |                 |         |               |
      |               |         |                 |   Tx    |               |
      |               | Rx vIRQ |                 |<--------|               |
      |gunyah_msgq_recv() |<--------|Message queue 2  | Tx vIRQ |gunyah_msgq_send() |
      |               |         |                 |-------->|               |
      |               |         |                 |         |               |
      |               |         |                 |         |               |
      +---------------+         +-----------------+         +---------------+
