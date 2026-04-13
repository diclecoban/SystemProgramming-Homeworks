# CSE 344 Homework 3 Report

## Introduction

This homework implements a multi-process word transportation and concurrent sorting system in C on a POSIX-compatible environment. The system reads words from an input file, admits them to arrival floors, transports characters one by one to their sorting floors, and reconstructs the original words concurrently.

The main challenge is keeping correctness under concurrency. Multiple independent processes may try to claim words, claim character tasks, sort the same floor, or move carriers at the same time. The design therefore focuses on shared-memory synchronization, atomic admission control, one-time character delivery, and clean termination.

## System Architecture

The system contains these process groups:

- Parent/coordinator: parses arguments, reads input, initializes shared memory, creates child processes, monitors completion, writes the final output file, and performs shutdown.
- Word-carrier processes: fixed to one arrival floor, scan the input list in synchronized round-robin order, claim an available word, and atomically check both arrival and sorting floor capacities.
- Letter-carrier processes: mobile workers that live on a current floor, claim one character task at a time, request the delivery elevator when destination differs, or place the character directly if the sorting floor is the same.
- Sorting processes: fixed to one sorting floor and allowed to sort any word assigned to that floor, while a per-word sorting lock ensures only one sorter works on a specific word at a time.
- Delivery elevator: serves character delivery requests through an independent queue.
- Reposition elevator: serves idle letter-carrier relocation requests through a separate queue.

## Data Structures and Shared Memory Design

The implementation uses one `mmap(..., MAP_SHARED | MAP_ANONYMOUS, ...)` region holding all shared state. Important structures are:

- `SharedWord`: stores `word_id`, original word, `sorting_floor`, `arrival_floor`, `claimed`, `admitted`, `completed`, `sorting_area[]`, `sorting_meta[]`, `occupied[]`, `fixed[]`, and per-character claim/delivery flags.
- `FloorState`: stores the number of active words and current letter-carrier count on each floor.
- `CarrierState`: stores the current floor and per-carrier private event semaphore.
- `DeliveryRequest` and `RepositionRequest`: queue entries used by the two elevator processes.
- Global counters: retries, completed words, transported characters, and elevator operation counts.

Each word also has:

- a `mutex` semaphore for shared updates,
- a `sorter_lock` semaphore to prevent concurrent sorting of the same word.

## Synchronization Strategy

Synchronization is based on POSIX semaphores. The shared state stores semaphore references used by all forked child processes:

- `global_mutex` protects global round-robin state and shared slot allocation.
- per-floor mutexes protect `active_words` and `carrier_count`.
- per-word mutexes protect claim flags, delivery state, sorting area updates, and completion flags.
- per-word sorter locks ensure only one sorting process operates on a word at a time.
- `delivery_mutex` and `reposition_mutex` protect the two request queues.
- counting semaphores `delivery_items` and `reposition_items` wake elevator processes only when there is queued work.
- each letter-carrier has an `event_sem` used by elevators to wake the carrier after delivery or reposition.

This prevents duplicate claims, inconsistent floor-capacity reservations, and concurrent modification of the same word.

## Word Admission Logic

Word-carrier processes share a synchronized round-robin cursor. A carrier scans from the current cursor position, atomically claims the first available word, and then locks the relevant floor states in increasing floor order to avoid deadlock.

Admission succeeds only if both:

- the arrival floor has free capacity,
- the sorting floor has free capacity.

If either check fails, the carrier releases the word and increments the retry counter. If both succeed, the word becomes admitted, its arrival floor is set, and both floor capacities are reserved together.

## Character Transportation and Elevators

For an admitted word, each character becomes an implicit task defined by its original index in the word. A letter-carrier scans admitted words on its current floor and claims exactly one not-yet-claimed, not-yet-delivered character.

Transport rules:

- if the destination floor equals the current floor, the carrier directly places the character into the first empty non-fixed slot of the sorting area;
- otherwise, it sends a delivery request to the delivery elevator and waits on its private event semaphore;
- after delivery, the carrier continues from the destination floor;
- if no work exists on its current floor, it requests the reposition elevator and resumes from a new floor.

The two elevator processes use separate request arrays and separate semaphores, so they operate independently.

## Sorting Algorithm

Characters are not inserted directly into their final index. Instead, they are first placed into the first suitable non-fixed empty slot in `sorting_area[]`. The associated `sorting_meta[]` records the character’s original index in the word.

A sorting process then scans the word:

- if a slot is empty, it skips it;
- if the slot is already fixed, it leaves it untouched;
- if the stored original index equals the current slot index, it marks that index as fixed;
- if the target slot is empty, it moves the character there;
- if the target slot is occupied and not fixed, it swaps;
- if the target is fixed, it leaves the character for a future pass.

Because each character carries its original index, repeated letters are reconstructed correctly.

## Termination Detection

Completion of a word is detected when all `fixed[]` entries become `1`. At that point:

- the word’s `completed` flag is set,
- the global completed-word count is incremented,
- reserved capacity on arrival and sorting floors is released.

The parent monitors the global completed count. When all input words are completed, it sets shutdown, wakes waiting processes, generates the final output file, prints summary statistics, and waits for all children.

The program also installs `SIGINT` and `SIGTERM` handlers so the shared shutdown flag is set on Ctrl+C, allowing a controlled cleanup path.

## Output File Generation

The parent creates the output file only after all words are complete. It sorts pointers to shared words using:

1. `sorting_floor`
2. `word_id`

Then it writes each line in the required format:

`<word_id> <word> <sorting_floor>`

## Test Case Scenario

Mandatory-style sample input used for validation:

```text
101 apple 3
102 process 1
103 system 2
104 kernel 0
105 thread 1
106 memory 3
107 signal 2
108 mutex 0
```

Example run command:

```bash
./hw3 -f 4 -w 2 -l 1 -s 2 -c 3 -d 3 -r 2 -i sample_input.txt -o output.txt
```

Observed behavior:

- multiple floors are initialized with multiple process types,
- words are admitted only when both capacities are available,
- letters are delivered individually,
- sorting proceeds concurrently with ongoing transportation,
- the final file is ordered by sorting floor and word id.

## Challenges and Solutions

The hardest parts were coordinating several process types without duplicate work and keeping the system moving without corrupting shared state.

Main solutions:

- ordered floor locking during admission to avoid deadlock,
- per-word mutexes for character claim and sorting-area updates,
- per-word sorter lock to enforce exclusive sorting per word,
- separate elevator queues and wake-up semaphores,
- shared completion counters for parent-side termination detection.

## Conclusion

This design satisfies the assignment’s central requirement: multiple independent processes operate concurrently on shared data while maintaining synchronization, consistency, and coordinated shutdown. The implementation is intentionally modular enough to explain clearly during demo and to extend with more advanced scheduling policies later.
