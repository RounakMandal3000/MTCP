# MTCP (Mini Transport Protocol over UDP)

A minimal reliable message transfer layer built on top of UDP. It implements a sliding window, cumulative acknowledgments, basic loss handling (with simulated drop probability), shared-memory based socket table, and automatic garbage collection of orphaned sockets.

> Educational project: not production-hardened. Focus is clarity of mechanisms (windowing, ordering, retransmission) rather than performance or full RFC compliance.

---
## ✨ Key Features
- Custom socket API: `m_socket`, `m_bind`, `m_sendto`, `m_recvfrom`, `m_close` (mirrors BSD style, type = `SOCK_MTP`).
- Shared memory segment (System V) used as a global socket manager across processes.
- Sender & Receiver background threads (spawned by `initmsocket` process) multiplex all active MTP sockets using `select()`.
- Fixed message size (1 KB) and 4-bit circular sequence space (0–15).
- Sliding windows (configurable sizes: `SENDER_SWND`, `RECV_SWND`) with ordered insertion and cumulative ACKs.
- Timer-based retransmission (`T` seconds) + simulated packet loss via `DROP_PROB`.
- Garbage Collector (GC) thread that reclaims sockets whose owning process has terminated unexpectedly.
- Colorful structured logging with easily customizable log macros.

---
## 🧱 Architecture Overview

```
+----------------+          +----------------+
|  user1 (sender)|          | user2 (receiver)|
|  m_socket()    |          |  m_socket()     |
|  m_bind()      |          |  m_bind()       |
|  file_to_sender|          |  receiver_to_   |
|  thread()      |          |  file_thread()  |
+--------+-------+          +---------+-------+
         |                              |
         | MTP API calls (shared memory) |
         v                              v
   +------------------------------------------+
   |   Shared Memory (MTP_SM)                 |
   |  - Socket table (MAX_SOCKETS)            |
   |  - Sender/Receiver buffers               |
   |  - r_ack[] (next expected seq per slot)  |
   |  - Mutex per slot + global lock          |
   +--------------------+---------------------+
                        |
                        | accessed by PID: initmsocket
                        v
              +--------------------------+
              | initmsocket process      |
              |  Threads:                |
              |   - receiver_thread      |
              |   - sender_thread        |
              |   - garbage_collector    |
              +--------------------------+
                        |
                        | UDP datagrams
                        v
                  +-----------+
                  |   UDP     |
                  +-----------+
```

---
## ⚙️ Build & Run

### Prerequisites
- Linux or WSL environment (System V shared memory + POSIX threads required)
- `gcc` (or compatible C compiler)
- Make (optional if using provided `makefile.mak`)

### Build
```
make -f makefile.mak init   # builds the library/object files (adjust target if needed)
```
OR manually:
```
gcc -Wall -Wextra -pthread -c msocket.c
gcc -Wall -Wextra -pthread -c initmsocket.c
```
You typically run the `initmsocket` executable first (if you have a separate runner; otherwise integrate its logic into your launch scripts).

### Running Example (Two Terminals)
1. Terminal A – start the manager & sender:
```
./initmsocket   # if compiled as standalone; otherwise ensure it is running somehow
./user1 127.0.0.1 5001 127.0.0.1 5002 large_file.txt
```
2. Terminal B – start the receiver:
```
./user2 127.0.0.1 5002 127.0.0.1 5001
```
Data received will append to `receiver_buffer.txt`; sent lines logged to `sender_buffer.txt`.

> Adjust ports to avoid conflicts. Multiple pairs can coexist if they choose non-overlapping ports.

---
## 📦 Custom Socket API
| Function | Description |
|----------|-------------|
| `m_socket(domain, SOCK_MTP, 0)` | Allocates a slot in shared memory; underlying UDP socket created lazily by init thread. |
| `m_bind(sockfd, src, dest, len)` | Registers local/remote addresses and triggers OS-level bind in manager loop. |
| `m_sendto(sockfd, msg, len, ...)` | Enqueues a message into per-socket sender buffer (if space). |
| `m_recvfrom(sockfd, buf, ...)` | Delivers next in-order message (blocks at higher level by polling). |
| `m_close(sockfd)` | Closes UDP fd, resets slot state. |

### Message Structure (`MTP_Message`)
- `seq_num` – 4-bit sequence (0–15)
- `is_ack` – data vs ACK
- `next_val` – cumulative next required seq (carried in ACKs)
- `data[1024]` – payload
- `sent_time` – monotonic timestamp for retransmission logic

---
## 🔁 Sliding Window & Reliability
- Sender Window (`SENDER_SWND`): Tracks unacknowledged packets; timeout-based retransmit after `T` seconds.
- Receiver Buffer (ordered insertion): Accepts in-window packets, computes next contiguous expected sequence.
- ACK Strategy: Each received data packet triggers an ACK with `next_val = first missing in-order seq`.
- Duplicate / out-of-window packets may be dropped or provoke duplicate ACK (if you extend logic further).

---
## 🗃 Garbage Collection
The GC thread reclaims sockets when:
1. Slot is not free AND
2. `kill(pid_creation, 0)` returns ESRCH (process no longer exists)

Actions on reclaim:
- Close underlying UDP fd
- Reset sender/receiver buffers
- Reset r_ack[] entry
- Mark slot free

Limitations:
- PID reuse race possible; mitigations (not implemented) include pairing PID with start time.

---
## 🪵 Logging System
Defined in `msocket.h`:
- Levels: INFO, WARN, ERR, DEBUG
- Colorful ANSI output (disable with `#define MTP_NO_COLOR` before including header)
- Buffer snapshots: `mtp_print_window_sender`, `mtp_print_buffer_sender`, `mtp_print_buffer_receiver`

Example:
```
[INFO] socket slot=2 reserved (pid=12345)
[INFO] tx new seq=03 slot=2
[DBG ] rx DATA seq=03 from=127.0.0.1
[INFO] sent ACK ack.next=04 ack.seq(for ref)=03
[WARN] retransmit seq=04 slot=2
```

---
## 🧪 Simulated Loss
- Controlled by `DROP_PROB` macro (default 0.3)
- Modify in `msocket.h` and rebuild
- Used only in receiver path to simulate unstable network conditions

---
## 🔧 Configuration Macros
| Macro | Meaning | Default |
|-------|---------|---------|
| `MTP_MSG_SIZE` | Message payload bytes | 1024 |
| `SEQ_BITS` | Sequence number bit width | 4 |
| `SENDER_BUFFER` | Queue size before entering send window | 10 |
| `SENDER_SWND` | Sender sliding window | 5 |
| `RECV_BUFFER` | Receiver hold buffer | 5 |
| `RECV_SWND` | Receiver window span | 5 |
| `T` | Retransmit timeout (s) | 5 |
| `DROP_PROB` | Simulated loss probability | 0.3 |

---
## 🚧 Limitations & Future Ideas
- Fixed-size messages (no fragmentation / streaming reassembly)
- No congestion control
- No selective ACK / fast retransmit heuristics
- No encryption or integrity check (CRC / HMAC)
- GC may mis-hold a slot briefly if PID reused quickly

Potential enhancements:
- Add fast retransmit on 3 duplicate ACKs
- Switch from polling sleeps to condition variables for better CPU efficiency
- Implement inactivity-based GC (idle timeout)
- Extend API with non-blocking `m_recvfrom` or `m_select` wrapper

---
## 🛠 Development Tips
- Use `MTP_LOG_DEBUG` generously when adding features; wrap with build flag to disable in release.
- To change verbosity, redefine macros or add an environment-variable gate.
- Redirect logs: `./user1 ... 2> sender.log`

---
## 📂 Repository Layout
```
msocket.h        # Public API + structs + logging
msocket.c        # API implementations + sender/receiver helper threads
initmsocket.c    # Initializes shared memory + global threads (rx, tx, GC)
user1.c          # Sample sender application (reads file -> protocol)
user2.c          # Sample receiver application (writes to file)
large_file.txt   # Example input file
sender_buffer.txt / receiver_buffer.txt # Trace outputs
makefile.mak     # Build script
```

---
## 🧾 License
Add an explicit license (e.g. MIT) if you plan to share publicly.

---
## 🙌 Attribution / Purpose
Built as a learning exercise to understand fundamentals of reliable transport over unreliable datagrams: buffering, ordering, timing, shared memory coordination, and protocol state maintenance.

Feel free to open an issue / PR with improvements or questions.

Happy hacking! 🚀
