# Library Book Lending Tracker: how it works

## The target goal

A library's lending log can be rewritten by whoever controls it: a librarian can quietly mark a lost book as returned, a borrower can dispute what the log shows, and a due date can be moved after the fact. This tracker records every lending event as a signed record on a blockchain, so that:

- nobody can change a confirmed record without it being detected (SHA-256 hashes, a Merkle root per block, proof of work)
- every record proves who asked for it (an ECDSA signature by the member, or by the librarian for what the library observes)
- a signed request cannot be submitted twice (a per-signer nonce)
- lending rules are enforced the same way when a request is made, when it is mined and when the chain is replayed
- returning books on time earns tokens, and fines are paid in tokens, under either a UTXO or an account model
- the whole state survives restarts, and edited files are noticed

## Module overview

| File | Responsibility |
|---|---|
| `main.c` | command-line options, one-shot mode or the interactive shell |
| `cli.c` | command table, argument splitting with double quotes, usage messages, `help` |
| `catalog_commands.c`, `lending_commands.c`, `token_commands.c`, `mining_commands.c`, `fraud_commands.c`, `audit_commands.c`, `chain_commands.c` | one handler per command, grouped as in `help` |
| `library.c` | startup, genesis block, building the effective state, submitting requests, pruning the pool |
| `state.c` | the lending rules (`apply_record`), block reward transactions, replaying the chain |
| `registry.c`, `inventory.c` | books, members and copies; the copy state machine |
| `reservation.c` | reservation queues (linked lists) |
| `record.c` | the signed record: payload, ID, signature, file line |
| `merkle.c` | Merkle root and tree printing |
| `blockchain.c` | blocks, proof of work, validation, `chain.dat` |
| `pool.c` | pending requests, ordering, `pool.dat` |
| `fraud.c` | the misuse heuristics |
| `transaction.c`, `utxo.c`, `account.c`, `ledger.c` | token transactions and the two ledger models |
| `mining.c` | assembling a block from the pool and committing it |
| `keyring.c`, `crypto.c` | identities, unlocked keys, OpenSSL hashing, signing and key files |
| `storage.c` | snapshots and `meta.dat` |
| `input.c`, `utils.c` | hidden passphrase input, Ctrl-C handling, parsing and formatting helpers |

## One source of truth

The chain is the only thing trusted. Copy states, reservation queues, fines, identities, nonces and token balances are all a result of applying the confirmed records in order, and the program rebuilds them from the chain every time it starts.

All lending rules live in one function, `apply_record` in `state.c`. It checks that the signer is registered and used their next nonce, checks the action against the current state, and only then changes anything. The same function is used three ways:

| When | Applied to | Purpose |
|---|---|---|
| a request is made | the confirmed state plus every pending request (the effective state) | refuse requests that could never be confirmed |
| a block is mined | the confirmed state plus the records already chosen for the block | choose which requests go in the block |
| the program starts | an empty state, record by record from the genesis block | rebuild the state and catch any record that breaks a rule |

Because the three uses share one rule set, they cannot disagree. A record with a valid signature that breaks a rule (for example a fine that does not match the due date) is caught by the replay, and the chain is then treated as invalid.

## Blocks and records

### Block

```c
typedef struct {
    int index;
    time_t timestamp;
    int record_count;
    char previous_hash[HASH_HEX_SIZE];
    char merkle_root[HASH_HEX_SIZE];
    char hash[HASH_HEX_SIZE];
    char sealed_by[ID_SIZE];
    int token_reward;
    char transaction_id[HASH_HEX_SIZE];
    int difficulty;
    unsigned long nonce;
    int block_reward;
    TxOutput payouts[MAX_PAYOUTS];
    int payout_count;
    Record records[RECORDS_PER_BLOCK];
} Block;
```

The first nine fields are the required ones. `difficulty` and `nonce` are the proof of work, and `block_reward` with `payouts` records who received the 50 coin mining reward, so a pool's split can be rebuilt from the chain.

A block holds up to `RECORDS_PER_BLOCK` (4) records. Batching is by fixed size: a miner takes the top 4 requests from the pool, and a block can hold fewer when fewer are ready.

The block hash is SHA-256 over every header field except the hash itself. The records are covered through `merkle_root`.

### Record

| Field | Content |
|---|---|
| `record_id` | SHA-256 of every field except the signature (this is also the request ID) |
| `action` | BORROWED, RETURNED, RESERVED, RENEWED, OVERDUE, LOST, REGISTERED, TRANSFER or FINE_PAID |
| `signer_id`, `nonce`, `timestamp` | who signed, their nonce, when |
| `book_id`, `copy_id`, `book_title` | the copy, with the title copied from the registry |
| `member_id`, `member_name` | the member, with the name copied from the registry |
| `due_date`, `fine_amount` | loan dates and fines |
| `condition`, `reason_code` | the condition a copy came back in, and the librarian's reason code |
| `recipient_id`, `amount` | token payments |
| `public_key_hex` | the key a REGISTERED record puts on the chain |
| `signature` | DER-encoded ECDSA signature, up to 72 bytes |

The five lending actions come from the required record layout. Four more are needed by other requirements:

- REGISTERED carries a member's public key onto the chain, so the key can serve as their on-chain identity.
- LOST is the librarian's override for a lost copy, with a reason code.
- TRANSFER and FINE_PAID are token payments, which must go through the pending pool like every other transaction.

Fields an action does not use are left empty.

`record_id` deliberately leaves out the signature. An ECDSA signature can be re-encoded into a different valid signature without the private key, and if the ID covered the signature, the same request could appear under two IDs. This is the problem Bitcoin's transaction IDs had before SegWit.

## Merkle tree

Each block stores the Merkle root of its records, built in `merkle.c` without any library:

1. Each leaf is the SHA-256 of a record's payload and signature.
2. Neighbouring hashes are concatenated and hashed to make the level above.
3. A node left without a partner is paired with itself, as in Bitcoin.
4. This repeats until one hash, the root, is left.

`chain_view <index>` prints the tree level by level:

```
  merkle tree:
    leaves   27bed5db6ca0 2d9228a39cdc be5d02726b2f 2ed0795da7c4
    parents  768815cfb9a8 76411edce999
    root     19c9f61c3f54
```

Validation rebuilds the root from the records and compares it with the stored one. Editing any field of any record, or swapping a signature, changes that record's leaf and therefore the root.

Pairing an odd node with itself has a known weakness: the records `[a, b, c]` and `[a, b, c, c]` produce the same root. Two things close it:

- `record_count` is part of the hashed header, so adding a copy of the last record changes the block hash.
- A block that contains the same record twice fails validation.

## Identities and ECDSA signatures

### Keys

- Every identity has its own secp256k1 key pair, generated with OpenSSL's `EVP_EC_gen`. The identities are the librarian and each member; members also mine.
- `EVP_PKEY` is OpenSSL 3's replacement for the older `EC_KEY` API.
- The private key is saved in `keys/<ID>_private.pem`, encrypted with AES-256-CBC under the owner's passphrase (at least 8 characters, typed twice), with file mode 0600 in a 0700 folder.
- The public key is saved next to it in `keys/<ID>_public.pem`.

### The public key is the identity on the chain

The public key does not have to be trusted from a file. It is written onto the chain as hex in a REGISTERED record, and every later signature is verified against the key on the chain.

- **The genesis block's only record** is the librarian registering their own key. That key is the trust anchor; at startup it is also compared with `keys/LIBRARIAN_public.pem`.
- **Member keys are created on demand.** The first time a member signs anything, the program creates their key pair and the librarian signs a REGISTERED record for it. Requiring the librarian's signature stops anyone from claiming another member's ID first.

```
library> borrow_book BK003 ALU001
ALU001 has no key on the chain yet. Their key pair is created now and the librarian signs the registration.
Creating a secp256k1 key pair for ALU001.
New passphrase for ALU001: Confirm passphrase: Saved keys/ALU001_private.pem (encrypted with the passphrase) and keys/ALU001_public.pem.
Request 539c0c2747a2 added to the pending pool: REGISTERED signed by LIBRARIAN with nonce 3.
Request b1ac3f6601ef added to the pending pool: BORROWED signed by ALU001 with nonce 0.
```

### Who signs what

| Signed by the member | Signed by the librarian |
|---|---|
| BORROWED, RESERVED, RENEWED, TRANSFER, FINE_PAID | RETURNED, OVERDUE, LOST, REGISTERED |

Members sign what they ask for. The librarian signs what the library observes: a copy coming back and its condition, a loan passing its due date, a copy going missing, a new identity. The rule is checked during validation too, so a member cannot sign a RETURNED record.

### Signing and verification

- **Signing:** the record's payload (every field except the ID and the signature, joined with `|`) is signed with ECDSA over SHA-256 using `EVP_DigestSign`.
- **Verification:** `EVP_DigestVerify` checks the signature with the signer's public key from the chain.
- **When records are verified:** before a request enters the pool, again when a miner picks it, and for every record when the chain is validated. A record with a missing or invalid signature is rejected each time.
- **Signature length:** a DER signature's length varies (70 to 72 bytes on this curve), but the second byte of the encoding gives it: the total is that byte plus 2. The program reads it from there, after checking that the first byte is `0x30` and the length fits, because signatures are read back from files that could be edited. No separate length field is stored.
- **Keys in memory:** a private key is only in memory after its owner types the passphrase, and `logout` (or exit, including Ctrl-C) frees every unlocked key. The decrypted key must match the public key registered on the chain.

## Nonces and replay protection

Every identity has a nonce that starts at 0. A request must carry exactly the signer's next nonce, and the nonce only advances when the request is confirmed in a block.

- **When a request is made,** the next nonce is the confirmed one plus the signer's requests still pending, so a member can have several requests waiting.
- **When mining,** the miner takes the pool in priority order, but a request whose nonce is not the signer's next one yet is left for a later pass or block. That keeps one member's requests in nonce order even when priority would reorder them.
- **A reused or skipped nonce is refused,** with an explicit reason:

```
library> account_transfer ALU001 ALU006 1 0
REJECTED: nonce 0 from ALU001 was already used, the next is 5
library> account_transfer ALU001 ALU006 1 9
REJECTED: nonce 9 from ALU001 skips nonce 5, which was never confirmed
```

**Nonce gaps.** When a pending request is rejected or dropped, the same signer's later requests skip its nonce and can never be confirmed. The pool is re-checked at startup, after every block and after every review decision, and those requests are dropped with an explanation:

```
Dropped request 2c975c23a89e (RENEWED by ALU001): nonce 5 from ALU001 skips nonce 4, which was never confirmed
  An earlier request from ALU001 was rejected or dropped, leaving a nonce gap. Submit this one again.
```

## Copy inventory

`copies.txt` lists each physical copy (`BK001-1,BK001,GOOD`). A title can be partly available: `view_book` shows "1 of 2 available" when one copy is out.

```c
typedef struct {
    char copy_id[ID_SIZE];
    char book_id[ID_SIZE];
    CopyCondition condition;      /* NEW, GOOD, WORN or LOST */
    CopyState state;              /* AVAILABLE, BORROWED, RESERVED or LOST */
    char current_holder_id[ID_SIZE];
    time_t due_date;
    int renewal_count;
    bool overdue_recorded;
    time_t reserved_at;
} Copy;
```

### State machine

Every state change goes through `change_copy_state`, which refuses any move that `transition_is_allowed` does not list:

```
            borrow                    return, queue empty
AVAILABLE ---------> BORROWED ---------------------------> AVAILABLE
    |                 |   ^
    |                 |   |  the member it is held for borrows it
    |                 |   |
    |                 v   |
    |                RESERVED   <- return while someone is waiting
    |                 |
    +-----> LOST <----+  (only from AVAILABLE or BORROWED, with a reason code)
```

RESERVED to BORROWED is not in the original transition list. It is how the member at the front of a queue collects the copy held for them, and only that member can make it. LOST is final.

Return conditions can only stay the same or get worse (NEW, then GOOD, then WORN). A drop of more than one level needs a reason code, otherwise the return is flagged as suspicious.

## Reservations

A member can reserve a title only when none of its copies is available. Each title has its own first-in, first-out queue, a singly linked list with head and tail pointers, holding the member ID and the reservation date.

When a copy of that title is returned, it is offered automatically: the member at the head of the queue is removed from the queue and the copy becomes RESERVED for them, before anyone else can borrow it.

```
library> reservation_status BK003
No one is waiting for BK003.
  copy BK003-1 is held for ALU006 to collect with borrow_book BK003 ALU006
```

A renewal is refused while anyone is waiting for the title, so a reservation cannot be blocked by renewing forever.

## Due dates, renewals, overdue detection and fines

| Rule | Value |
|---|---|
| loan period | 14 days |
| renewal | 7 more days, at most 2 renewals, not while someone is waiting, not once overdue |
| fine | 2 coins per started day late |

A day is `DAY_SECONDS` long: 86,400 normally, and a test build can set it to 10 with `-DDAY_SECONDS=10`.

**Overdue detection** runs before every command. It looks for borrowed copies past their due date that have no OVERDUE record yet. If the librarian's key is unlocked, it signs an OVERDUE request for each, with the fine so far. If not, it prints a reminder to run `check_overdue`, which asks for the librarian's passphrase:

```
library> check_overdue
Passphrase for LIBRARIAN: LIBRARIAN unlocked.
Overdue: BK002-1 held by ALU002, fine so far 6.
Request 88b9d189deef added to the pending pool: OVERDUE signed by LIBRARIAN with nonce 6.
```

**The fine becomes owed when the late return is confirmed.** The RETURNED record carries `fine_amount`, and the rules check it against the due date and return time, so neither side can change it. A member with unpaid fines cannot borrow. `settle_fine` pays the whole fine to the LIBRARY account in tokens, as a signed FINE_PAID request.

## The pending pool

Every request (lending, reservation, renewal, payment, registration) is checked, signed and added to the pool. Nothing changes until it is mined.

| Pool field | Where it comes from |
|---|---|
| `request_id` | the record's `record_id` |
| `book_id`, `copy_id`, `member_id`, `action_requested` | the record |
| `priority` | high for overdue-related requests (an OVERDUE record, a late return, a fine payment), normal for the rest |
| `order_time` | the member's reservation date when they are collecting a reserved copy or waiting in the queue, otherwise the time of the request |
| `status` | PENDING, SUSPICIOUS or APPROVED |

**Mining order** is a sort of the pool array: high priority first, then `order_time` ascending, so the member who has waited longest is served first; ties keep submission order. It's an insertion sort over the pool's indices, because the pool stays small and is kept in submission order. `pool_view` shows the pool in that order:

```
library> pool_view
Pending pool: 2 requests, in the order a miner takes them
  #   request_id   status     action     prio  nonce  order time          subject
  1   88b9d189deef PENDING    OVERDUE    high  6      2026-10-08 22:42:50 BK002-1 for ALU002
  2   46269bcb2b79 PENDING    TRANSFER   normal 0      2026-10-08 22:40:06 ALU005 pays 10 to CLOUD-PROVIDER
```

**After a block is mined,** its requests leave the pool, and the rest are re-checked against the new state.

## Misuse detection and review

Four heuristics run on every request. A match marks it SUSPICIOUS:

| Heuristic | Condition |
|---|---|
| Excessive concurrent borrows | the member would hold more than 3 copies |
| Rapid re-borrow | the member borrows a title again within one day of returning it |
| Duplicate request | the request ID is already on the chain or in the pool |
| Condition manipulation | a return lowers the condition by more than one level without a reason code |

```
library> borrow_book BK005 ALU001
Request 1f68bc899ad1 added to the pending pool: BORROWED signed by ALU001 with nonce 5.
FLAGGED SUSPICIOUS: excessive borrowing: would hold 4 copies, the limit is 3
It stays in the pool and will not be mined until a librarian runs approve_suspicious or reject_suspicious.
```

**What happens to a flagged request:**
1. It stays in the pool and no miner ever takes it.
2. `fraud_review` lists it with its reason.
3. The librarian decides with `approve_suspicious <id>` or `reject_suspicious <id>` (the ID or its first 6 characters).
4. Rejecting it removes it, and the nonce gap rule then drops the signer's later requests.
5. Approving it only lifts the hold: the request is checked again like any other.

That last point is why `replay_request <id>`, which resubmits an already signed request byte for byte, is safe even if a librarian mistakenly approves the replay. It is flagged as a duplicate, and after approval it is dropped because its nonce is already used.

**A flagged request keeps its nonce,** so the signer's later requests wait until it is reviewed. For a flagged return, the signer is the librarian.

## Token rewards and ledgers

### Rewards

When a block is confirmed, its reward transaction pays every member who returned a book in it:

| Return | `token_reward` | Member receives |
|---|---|---|
| on time | 10 | 9 |
| late (with a fine) | 5 | 4 |
| not returned (only OVERDUE) | 0 | no transaction |

**Fee:** a fee of 1 coin is taken from each reward. Fees are burned: they leave circulation and nobody receives them.

**What the block records:**
- `token_reward` is the block's total.
- `transaction_id` is the SHA-256 of the reward transaction. That transaction is built only from the block's records, so validation rebuilds it and compares.

**The mining transaction** pays the 50 coin block reward according to `payouts`.

**The order inside a block:** the records' own payments are applied first, then the reward, then the mining payout. Rewards in a block can therefore only be spent in a later block.

### Every payment is on the chain

Transfers and fine payments are signed records mined like everything else. Formative 2 confirmed transfers immediately and kept them in a separate `transactions.txt`, which nothing on the chain protected. Now nothing outside the chain decides a balance, and both models rebuild balances from the same records.

### UTXO model

- Each payment's inputs are chosen when it is confirmed: the sender's oldest unspent outputs first, until they cover the amount plus the fee.
- Anything above that comes back as a change output.
- A payment whose inputs do not cover the amount plus fee is rejected.
- The whole set is printed after every confirmed block.

```
  7b169c0dbea6d6d0... 0      ALU006          3
  7b169c0dbea6d6d0... 1      ALU001          5      <- change from ALU001's 9 coin output
```

Validation of a UTXO payment requires every input to still be in the set, owned by the sender and used once. A spent input has left the set, which is what stops a double spend. `double_spend_test` tries to spend the last payment's inputs again:

```
Spending the inputs of payment e7cb73f27f5d again (ALU002, 2 inputs):
  input bf04236784d6884d...:0
  input cce634d8ed2e1dba...:0
REJECTED: input bf04236784d6...:0 is not in the UTXO set (already spent or never existed)
```

The signature covers who pays whom and how much, not the inputs. Because the inputs are chosen in a fixed order at confirmation, every replay of the chain picks the same ones. Bitcoin signs the inputs; here one signed record works in both models.

### Account model

- Each account has a balance and a singly linked history list (with a tail pointer, so adding is constant time).
- A payment debits the amount plus the fee and credits the recipient, and is rejected if the balance is short.
- The account's nonce is the member's signing nonce, which advances with every confirmed request they sign.
- `transaction_history` prints the list: sender, recipient, amount, fee, nonce, block and transaction ID.

```
library> transaction_history ALU002
Transaction history for ALU002: balance 7, next nonce 2
  block type     sender      fee nonce  txid           recipients
  6     REWARD   LIBRARY       1     -  bf04236784d6.. ALU002 4
  7     TRANSFER ALU005        1     1  cce634d8ed2e.. ALU002 10
  8     FINE     ALU002        1     1  e7cb73f27f5d.. LIBRARY 6
```

### Comparing the two

| Aspect | UTXO | Account |
|---|---|---|
| State | set of unspent outputs | balance per account |
| Balance | sum of the outputs a member owns | stored value |
| Double spend stopped by | spent inputs leaving the set | the nonce and the balance check |
| Change | explicit change output | not needed |
| Parallel validation | payments spending different outputs are independent | one sender's payments are ordered by nonce |
| History | spread across transactions | a linked list per account |
| Cost | more bookkeeping (input selection, change) | simple arithmetic |

The same chain opened with `--model utxo` and `--model account` gives the same balance for every account.

## Mining

### Proof of work

- **The search:** the miner increments `nonce` and recomputes the block hash until the hex hash starts with `difficulty` zeros.
- **Difficulty:** 1 to 4, set with `--difficulty` or `set_difficulty`. It is stored in each block, so a block is always checked against the difficulty it was mined at.
- **Cost:** each extra zero multiplies the expected attempts by 16.
- **Timestamps:** a block's timestamp is moved forward when needed so it is always later than the previous block's, because several blocks can be mined in one second.

### Solo

One member mines every pending request and receives the whole 50 coin block reward. Each block prints its nonce, attempts and time.

### Pool

- **Miners:** the member and three simulated peers (PEER-1 to PEER-3) get random hash rates from 50 to 300 attempts per round.
- **Fair turns:** each round is split into 10 time slices, and in each slice every miner tries a tenth of its hash rate. Faster miners do proportionally more work, and no miner always goes first. They share one nonce sequence, so no work is repeated.
- **Payout:** the 2% pool fee (1 coin) goes to POOL-OPERATOR. Each miner gets `attempts / total attempts` of the other 49, rounded down, and the remainder goes to the miner with the most attempts.
- **Payouts are sealed into the block.** They are part of the hashed header, so they have to be fixed before each hash. The pool recomputes the split, and the sealing miner, before every attempt from the attempts made so far. The block that meets the target therefore pays exactly for the work done up to that moment.

### Cloud

- **Payment:** a member rents 300 attempts per round for 1 to 5 rounds, at 5 coins per round. The whole rental is paid up front as one signed transfer to CLOUD-PROVIDER, so a member who cannot afford it is refused before any mining. The payment goes through the pool like any request; if the rental finds no block, the next miner confirms it.
- **Rewards:** a block found pays 45 coins to the renter and a 10% maintenance fee of 5 to the provider. An unfinished block carries over to the next round.
- **Reporting:** after each round the running rewards, fees and net are shown, with a warning whenever fees are higher. At the end there is a summary of gross earnings, fees and net profit.

At difficulty 4 a rental almost never finds a block:

```
Round 1: no valid hash in 300 attempts, block 4 carries over
  so far: rewards 0, fees 5, net -5
  WARNING: fees exceed rewards after round 1, the rental is unprofitable so far
...
Cloud mining summary
  Gross earnings  0 coins
  Fees paid       20 coins (rental 20 + maintenance 0)
  Net profit      -20 coins
```

## Validation and tamper detection

`validate_chain` checks every block:

| Check | Catches |
|---|---|
| `index` equals its position | inserted, removed or reordered blocks |
| timestamp later than the previous block's | backdated blocks |
| `previous_hash` equals the previous block's hash | broken links |
| stored hash equals the recomputed hash | any edited header field |
| hash meets the block's difficulty | blocks that skipped the proof of work |
| Merkle root rebuilt from the records | any edited record or signature, repeated records |
| reward fields rebuilt from the records; payouts add up to 50 | invented rewards |
| every record: ID, authorised signer, signature, nonce | forged or replayed records |

It also compares the chain with `meta.dat` to catch blocks deleted from the end. At startup the chain is validated and replayed before any command runs, and a chain that fails is kept for viewing only.

`tamper_demo` attacks a copy of the chain in memory. Step 1 quietly moves a loan's due date back 30 days:

```
Step 1: block 7, record 0 (BORROWED of BK001-1 by ALU001): due date moved from 2026-10-08 22:45:10 to 2026-10-08 22:50:10
Block 7    index OK   time OK   link OK   hash OK   pow OK   merkle FAIL reward OK   records FAIL <-- TAMPERED
           record 0 (5ec5b95e19f0...): record_id does not match the record's contents
```

The moved date shows 5 minutes because this run used 10-second days, so 30 days is 300 seconds.

Step 2 covers it up the way an attacker with the file could: recompute the record ID and the Merkle root, then redo the proof of work for that block and every block after it:

```
Re-mined 2 blocks in 355 hash attempts.
Block 7    index OK   time OK   link OK   hash OK   pow OK   merkle OK   reward OK   records FAIL <-- TAMPERED
           record 0 (fb200e39eef5...): signature INVALID for ALU001
```

Every hash, link, proof of work and Merkle root checks out again, but the signature does not, and re-signing needs ALU001's private key. Proof of work makes rewriting history expensive, and the signature makes it detectable at any cost.

## Persistence

**Format.** Everything is plain text with `|` between fields. Text was chosen over binary because writing structs with `fwrite` depends on compiler padding and the size of `time_t`, and because text can be inspected and edited by hand to test tamper detection. The registry loader refuses `|` in any field, so the separator cannot be injected.

**`chain.dat`** holds a `B` line per block, followed by exactly `record_count` `R` lines:

```
B|index|timestamp|record_count|previous_hash|merkle_root|hash|sealed_by|token_reward|transaction_id|difficulty|nonce|block_reward|payouts
R|record_id|action|signer|nonce|timestamp|book_id|copy_id|title|member_id|name|due_date|fine|condition|reason|recipient|amount|public_key|signature
```

A missing, extra or out-of-place line stops the load with its line number.

**The other files:**

| File | Lines |
|---|---|
| `pool.dat` | `P\|status\|priority\|order_time\|flag_reason`, then the request's `R` line |
| `inventory.dat` | `copy_id\|book_id\|condition\|state\|holder\|due_date\|renewals\|overdue_recorded\|reserved_at` |
| `reservations.dat` | `book_id\|position\|member_id\|reservation_date` |
| `ledger.dat` | `U` (UTXO) or `A` (account) lines, `N\|identity\|next_nonce`, `F\|member\|fines_owed` |
| `meta.dat` | `difficulty=`, `block_reward=`, `model=`, `height=`, `tip_hash=` |

**When it saves:**
- The chain and the pool are written whenever they change.
- The snapshots and `meta.dat` are written after every mined block, on `exit` and Ctrl-C, on `set_difficulty` and on `chain_save`.
- Every file is written to a temporary file and renamed, so a crash never leaves half a file.
- Ctrl-C only sets a flag; the main loop then saves, frees the keys and exits, with the terminal echo restored even if Ctrl-C came during a passphrase.

**At startup** each snapshot is compared with the state rebuilt from the chain. An edited snapshot is reported and rewritten from the chain:

```
WARNING: inventory.dat does not match the chain from line 6. It was changed outside the program; the state rebuilt from the chain is used and the file will be rewritten.
```

Deleting blocks from the end of `chain.dat` leaves a chain in which every remaining check passes. `meta.dat` remembers the height and the last block's hash, which exposes it:

```
WARNING: meta.dat was saved with 3 blocks but chain.dat now has 2: blocks were removed from the end, so the chain is INVALID.
```

`chain_save` writes everything at once, and `chain_load` reloads everything from disk and validates it again, as a restart would.

## Design choices and limitations

- **Miners are members.** Mining rewards are tokens, and spending tokens needs a signature, so a miner needs a key. Solo and cloud miners are members; the pool's three peers only receive payouts.
- **Pool fees are taken from the block reward;** transaction fees are burned.
- **One node.** There is no network or consensus between nodes; one copy of the chain is kept, protected by validation and `meta.dat`.
- **One trust anchor.** If the librarian's key leaked, the attacker could sign new records and register identities. Rotating the key would need a new record type.
- **A held copy waits for its member.** An offered copy stays RESERVED until that member borrows it; there is no expiry, and reservations cannot be cancelled.
- **A flagged librarian request delays the librarian.** It keeps its nonce, so the librarian's later requests wait for the review.

## Memory management

- **Growable arrays:** the chain, pool, UTXO set, account table, identities and reservation queues grow with `realloc`, doubling when full.
- **Linked lists:** the reservation queues and account histories are linked lists, freed node by node.
- **Copies of the state:** each one (made for every request and every block) is freed after use.
- **Keys:** every OpenSSL key and context is freed, and passphrase buffers are wiped with `OPENSSL_cleanse`.
- **Checked under sanitizers:** full sessions (registration, lending, all three mining methods, both models, fraud review, tamper demo, save and reload) ran under AddressSanitizer and UndefinedBehaviorSanitizer with no errors or leaks.

## Test results

| Test | Result |
|---|---|
| `borrow_book BK999 ALU001`, `borrow_book BK001 ALU999` | `ERROR: Book or Member not found`, nothing added |
| borrow with no copy available | `ERROR: no copy of BK003 is available. Join the queue with reserve_book BK003 ALU006` |
| return with a reservation queue | copy held for ALU006; other members refused |
| on-time return | ALU001 receives 9 |
| late return, 24 seconds late with 10-second days | fine 6, reward 4 |
| `settle_fine` with 4 coins against a fine of 6 | `REJECTED: insufficient funds: ALU002 has 4 coins ... needs 7 (6 + 1 fee)` |
| fourth concurrent borrow | flagged SUSPICIOUS, never mined; rejected by the librarian |
| reused and skipped nonces | rejected with the reason |
| double spend | rejected, input not in the UTXO set |
| same chain in both models | identical balances for every account |
| pool session | shares add to 49, 1 to POOL-OPERATOR |
| cloud rental at difficulty 2, then 4 | net +35; net -20 with a warning every round |
| tamper demo | step 1 caught by the Merkle root and record ID; step 2 only by the signature |
| edited `inventory.dat` and `ledger.dat` | reported and rewritten from the chain |
| last block deleted from `chain.dat` | reported, chain read-only, `meta.dat` not overwritten |
| Ctrl-C at the prompt | snapshots saved, keys freed, terminal restored |
