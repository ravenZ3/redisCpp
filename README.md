# Redis-Cpp - Tiny Redis Clone

this little redis clone knows a handful of commands, each one is like a thought in a stream, simple but purposeful:

## Commands

* **PING** → the heartbeat check. send `PING`, get `PONG`. simple, immediate.

* **ECHO <msg>** → speaks back what you say. say something, it says it back.

* **SET <key> <value> [PX <ms>]** → store a string under a key. optional PX lets it expire after milliseconds. like writing a note and setting a timer to burn it.

* **GET <key>** → read what you stored. returns `$-1` if nothing’s there.

* **LLEN <key>** → length of a list. zero if the list is empty or nonexistent.

* **LPUSH / RPUSH <key> <values...>** → add one or more values to the left or right of a list. returns new length.

* **LPOP <key> [count]** → remove and return elements from the left. supports popping multiple elements.

* **BLPOP <key> <timeout>** → blocking pop from the left. waits until an element is available or timeout expires. `timeout=0` waits indefinitely.

* **LRANGE <key> <start> <stop>** → get a slice of a list. handles negative indices like redis. returns in RESP2 array format.

* **TYPE <key>** → tells you the type of value stored at a key. returns `"string"`, `"list"`, or `"none"`.

basically, this is your tiny redis brain. it stores strings, lists, and knows how to wait, push, pop, slice, and tell you what it thinks it is.

## How it works — Multithreading & Parsing

so imagine the server is awake, listening. every client that knocks on the door gets its own little thread. it’s like giving each thought its own lane to move in, so nobody blocks anybody else.

* **threads** → each client lives in its own thread. do something long? doesn’t matter, others keep talking. `std::thread(DoWork, client_fd).detach();` — fire and forget, let it roam free.

* **mutexes** → shared memory is scary. strings, lists, anything in `m` or `list` is protected by a mutex. `std::lock_guard<std::mutex>` or `std::unique_lock<std::mutex>` makes sure no two threads fight over the same key at the same time. think of it as pausing one thought until another finishes its sentence.

* **condition variables** → this is for blocking pops. if a thread wants something from an empty list, it just waits. sleeps. doesn’t burn CPU. wakes up only when someone pushes. kind of like holding your breath until the world gives you an answer.

* **parsing RESP2** → the input isn’t plain text, it’s structured: `*3\r\n$5\r\nBLPOP\r\n$9\r\npineapple\r\n$3\r\n0.2\r\n`. your parser `f()` chops it into tokens, step by step.

  * first `*<count>` → how many parts.
  * then `$<len>` → grab exactly that many bytes.
  * repeat → get all command words.
  * now the server can understand what the client *actually wants*, instead of guessing.

basically, the server reads a message, parses it clean, locks memory if it needs to, performs the command, maybe waits if it’s a blocking operation, then sends a response - all while other threads do the same in parallel. chaos tamed by mutexes, order imposed by condition variables.
