# Blockchain-Based Library Book Lending Tracker

A command-line library lending system in C where every borrow, return, reservation, renewal, overdue flag and token payment is a signed record on a proof-of-work blockchain. Members earn tokens for returning books on time, pay fines in tokens, and every record can be checked by anyone holding the chain.

This is the complete version of the tracker. It combines and extends two earlier versions:

- [Formative 1](https://github.com/devark28/alu-blockchain-proj_1_assgmnt-library_lending): book and member registries, SHA-256 linked blocks, ECDSA signatures, chain validation and tamper detection.
- [Formative 2](https://github.com/devark28/alu-blockchain-proj_2_assgmnt-library_lending): token rewards, a pending pool, proof of work, UTXO and account ledgers, solo, pool and cloud mining.

A detailed explanation of the design, with real output from the program, is in [q1_library_lending/README.md](q1_library_lending/README.md).

Demo video: [https://youtu.be/qSCJqbQES_I](https://youtu.be/qSCJqbQES_I)

## What this version adds

| Area | Formative 2 | Now |
|---|---|---|
| Blocks | one lending record per block | up to 4 records per block, with a Merkle root of the records |
| Signatures | the librarian signs every record | every member has a key pair; members sign their own requests, the librarian signs what the library observes |
| Identity | none | each public key is registered on the chain in a REGISTERED record |
| Replay protection | nonce on transfers only | every signed request carries the signer's next nonce |
| Books | availability per title | copy-level inventory with a state machine (AVAILABLE, BORROWED, RESERVED, LOST) |
| Lending | borrow, return, overdue | adds reservations with a queue per title, renewals, fines, lost copies |
| Pending pool | first in, first out | sorted by priority (overdue first) and reservation date |
| Fraud | none | four misuse checks; flagged requests wait for a librarian's review |
| Tokens | transfers confirmed immediately, kept in `transactions.txt` | every payment is a mined record, so balances are rebuilt from the chain |
| Persistence | chain, pool and transaction log | chain, pool, inventory, reservations, ledger and chain metadata |
| Interface | numbered menu | named commands, in an interactive shell or one at a time |

## Requirements

- Linux (tested on Ubuntu 24.04)
- `gcc` (tested with 13.3)
- OpenSSL 3.0 or newer with development headers (tested with 3.0.13)
- `make`, for the Makefile (comes with `build-essential`; the `gcc` command below works without it)

```
sudo apt install build-essential libssl-dev
```

No other libraries are used. SHA-256 and ECDSA come from OpenSSL's libcrypto; the Merkle tree, UTXO set, account table, linked lists and reservation queues are written in C in this project.

## Build

From `q1_library_lending/`, with the Makefile:

```
make            build library_tracker
make test       build it with 10-second days (see below)
make clean      delete the program
make reset      delete the saved chain, pool, snapshots and keys, to start over
```

`make` runs exactly this `gcc` command, which also works on its own:

```
gcc -Wall -Wextra -o library_tracker *.c -lcrypto
```

It compiles with no warnings. A loan lasts 14 days, so to see overdue loans, fines and late returns without waiting, `make test` builds a version where one "day" lasts 10 seconds:

```
gcc -Wall -Wextra -DDAY_SECONDS=10 -o library_tracker *.c -lcrypto
```

Both `make` and `make test` always recompile, so switching between them never leaves the wrong build in place.

`DAY_SECONDS` sets the length of a day everywhere a day is used: the 14-day loan, the 7-day renewal, fines (2 coins per started day late) and the one-day window of the re-borrow check. A chain made with one build should be used with the same build.

## Run

```
./library_tracker                                  interactive shell
./library_tracker view_book BK003                  run one command and exit
./library_tracker --model account --difficulty 3   options go before the command
```

On the first run the program creates the librarian's key pair (it asks for a passphrase twice, at least 8 characters) and mines the genesis block. Type `help` in the shell for every command. Values with spaces go in double quotes:

```
library> register_book BK007 "Purple Hibiscus" "Chimamanda Ngozi Adichie"
```

Every command checks its arguments and prints a usage line when one is missing. Passphrases are always asked for with the terminal echo off, never taken as arguments.

### Switching between the UTXO and account models

```
./library_tracker --model utxo
./library_tracker --model account
```

The model is remembered in `meta.dat`, so later runs keep it until `--model` is given again. Every token movement is a record on the chain, so both models rebuild their balances from the chain at startup: switching models never loses or changes a balance. Commands that only make sense in one model (`utxo_view`, `double_spend_test`, `account_balance`, `account_transfer`, `transaction_history`) say which model they need.

### Setting the mining difficulty

The difficulty is how many leading zeros a block hash needs, from 1 to 4 (default 2):

```
./library_tracker --difficulty 4
library> set_difficulty 3
library> difficulty_status
```

`set_difficulty` needs the librarian's passphrase and is remembered in `meta.dat`. Each extra zero makes a valid hash 16 times rarer: about 16, 256, 4,096 and 65,536 attempts on average. `difficulty_status` shows those figures and the real attempt count of recent blocks.

## Commands

| Group | Commands |
|---|---|
| Membership | `register_member`, `view_member`, `member_nonce` |
| Books and copies | `register_book`, `view_book`, `add_copy`, `view_copy`, `mark_lost` |
| Lending | `borrow_book`, `return_book`, `renew_book`, `reserve_book`, `reservation_status` |
| Fines and overdue | `check_overdue`, `view_fines`, `settle_fine` |
| Tokens and ledger | `token_transfer`, `token_balance`, `utxo_view`, `account_balance`, `account_transfer`, `account_nonce`, `transaction_history`, `double_spend_test` |
| Blockchain and mining | `pool_view`, `mine_solo`, `mine_pool`, `mine_cloud`, `chain_view`, `validate_chain`, `chain_save`, `chain_load`, `difficulty_status`, `set_difficulty` |
| Fraud and audit | `fraud_review`, `approve_suspicious`, `reject_suspicious`, `replay_request`, `lending_history`, `member_history`, `book_history`, `tamper_demo` |
| Keys | `logout` |

## How to test the mining simulations

All three methods confirm the same pending pool. Put a few requests in it first:

```
library> borrow_book BK001 ALU001
library> borrow_book BK002 ALU002
library> pool_view
```

A member's first request creates their key pair (they choose a passphrase) and the librarian signs a REGISTERED record that puts the public key on the chain, so the first borrow by each member adds two requests.

**Solo mining.** One member mines every pending request and receives 50 coins per block:

```
library> mine_solo ALU005
Block 1: 4 records, nonce 1 found after 2 hash attempts
Block 2: 2 records, nonce 228 found after 229 hash attempts
Solo mining: 2 blocks, 231 hash attempts in total, 100 coins to ALU005.
```

**Pool mining.** The member and three simulated peers get random hash rates. Each block pays a 2% pool fee (1 coin) to POOL-OPERATOR and shares the other 49 coins by attempts:

```
library> mine_pool ALU005
Block 3 found by PEER-3 in round 1, nonce 99
  miner      hash rate   attempts    share  reward
  ALU005           140         28   28.00%      13
  PEER-1           130         26   26.00%      12
  PEER-2            60         12   12.00%       5
  PEER-3           280         34   34.00%      19
  total                       100  100.00%      49
```

**Cloud mining.** A member rents 300 attempts per round for 1 to 5 rounds at 5 coins per round, paid up front in tokens. A block found pays the renter 45 coins and the provider a 10% maintenance fee of 5:

```
library> mine_cloud 2 ALU005
Round 1: Block 4: 4 records, nonce 158 found after 159 hash attempts
  so far: rewards 50, fees 10, net 40
Round 2: no valid hash in 300 attempts, block 5 carries over
  so far: rewards 50, fees 15, net 35
```

To see an unprofitable rental, raise the difficulty first: at difficulty 4 a round of 300 attempts almost never finds a block, so every round prints `WARNING: fees exceed rewards`. The renter needs enough tokens for the rental; the rental is refused otherwise.

**Checking the result.** `validate_chain` checks every block, and `difficulty_status` lists the attempts each block took.

## Testing other behaviour

| What | Commands | Expected result |
|---|---|---|
| Invalid IDs | `borrow_book BK999 ALU001` | `ERROR: Book or Member not found`, nothing added |
| Reservation | borrow the only copy of BK003, then `reserve_book BK003 ALU006`, return and mine | the copy is held for ALU006; only ALU006 can borrow it |
| On-time and late returns | `return_book <copy> GOOD` before and after the due date, then mine | 9 coins (10 minus the fee), or 4 coins plus a fine |
| Overdue | wait past the due date, `check_overdue` | an OVERDUE request with the fine so far, ahead of everything else in `pool_view` |
| Fines | `view_fines`, `settle_fine <member>` | the fine is paid to LIBRARY in tokens; borrowing is blocked until then |
| Renewal | `renew_book <copy>` | due date extended by 7 days, at most twice, refused while someone is waiting |
| Fraud | a member borrows a fourth copy | `FLAGGED SUSPICIOUS`, listed by `fraud_review` until approved or rejected |
| Replay | `replay_request <id>` | flagged as a duplicate; if approved, dropped because its nonce is used |
| Wrong nonce | `account_transfer ALU001 ALU006 1 0` | `REJECTED: nonce 0 from ALU001 was already used` |
| Double spend | `double_spend_test` (UTXO model) | `REJECTED: input ... is not in the UTXO set` |
| Tampering | `tamper_demo` | after a cover-up only the member's signature fails |
| Edited files | change a line in `chain.dat`, `inventory.dat` or `ledger.dat` | reported at startup; an edited chain makes the program read-only |
| Missing blocks | delete the last block from `chain.dat` | `meta.dat` reports the missing blocks |

## Files

| File | Purpose |
|---|---|
| `books.txt`, `members.txt`, `copies.txt` | registries, one comma-separated record per line |
| `chain.dat` | the blockchain: each `B` line is a block, followed by its `R` record lines |
| `pool.dat` | pending requests: a `P` line with status and priority, then the `R` record line |
| `inventory.dat` | copy states, rebuilt from the chain and compared at startup |
| `reservations.dat` | reservation queues in order |
| `ledger.dat` | UTXO set or account balances, every signer's next nonce, unpaid fines |
| `meta.dat` | difficulty, block reward, model, chain height and the hash of the last block |
| `keys/` | one key pair per identity; private keys are encrypted with AES-256 and readable only by the owner |

The `.dat` files and `keys/` are created by the program. To start over, delete them with `make reset` (or `rm -rf *.dat keys/`).

## Screenshots

| File | Shows |
|---|---|
| [images/01_build_and_genesis.png](images/01_build_and_genesis.png) | compiling, the librarian's key pair and the genesis block |
| [images/02_registration.png](images/02_registration.png) | registering a member, a book and a copy |
| [images/03_invalid_ids.png](images/03_invalid_ids.png) | borrowing with an unknown book or member |
| [images/04_reservation.png](images/04_reservation.png) | reserving with no copy available, and the copy held after a return |
| [images/05_pool_priority.png](images/05_pool_priority.png) | the pending pool with an overdue request first |
| [images/06_solo_mining.png](images/06_solo_mining.png) | solo mining with hash attempts |
| [images/07_pool_mining.png](images/07_pool_mining.png) | the pool reward table |
| [images/08_cloud_mining.png](images/08_cloud_mining.png) | a cloud rental and its summary |
| [images/09_cloud_unprofitable.png](images/09_cloud_unprofitable.png) | an unprofitable rental with warnings |
| [images/10_return_rewards.png](images/10_return_rewards.png) | on-time and late returns and their rewards |
| [images/11_overdue_fines.png](images/11_overdue_fines.png) | overdue detection, fines and settling them |
| [images/12_renewal.png](images/12_renewal.png) | renewing a loan |
| [images/13_fraud_review.png](images/13_fraud_review.png) | a flagged request and the review commands |
| [images/14_merkle_block.png](images/14_merkle_block.png) | one block with its records and Merkle tree |
| [images/15_validate_tamper.png](images/15_validate_tamper.png) | chain validation and the tamper demo |
| [images/16_account_model.png](images/16_account_model.png) | balances, history and nonce rejections in the account model |
| [images/17_double_spend.png](images/17_double_spend.png) | the double-spend test in the UTXO model |
| [images/18_persistence.png](images/18_persistence.png) | startup reports for an edited snapshot and a shortened chain |
