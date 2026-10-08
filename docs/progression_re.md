# Credits (cR), rank and Armory: how the 360 `default.xex` does it

Target: Halo: Reach 360 1.0 (`/xbox360/default.xex`, title 4D53085B). Date: 2026-10-08.

Legend: **[C]** confirmed (seen in decompiled code, disassembly or file bytes). **[I]** inferred (naming, purpose or an untraced data path).
Internally Bungie calls credits **"cookies"** (MCC strings `rewards_get_cookie_total` and `not_enough_cookies` = "Not enough Credits").
The Bungie web service is named **"Omaha"**, the Reach codename (`/gameapi_omaha/...`).

All functions below are renamed in the Ghidra project. Several were missing from Ghidra and were created from `.pdata` bounds: 0x82605288, 0x8258F7F8, 0x826F3EA0, 0x82712EB0, 0x82711870, 0x82712490, 0x824BC138, 0x82110608, 0x82110740, 0x82590EE0, 0x8258F6E8.

## 0. TL;DR

1. **[C] The client owns the progression.** It stores the cR total, the per-Armory-item purchase flags, the unsynced deltas and the purchase log in a 0x548-byte "rewards block" inside the player profile blob.
   - That blob is saved as Xbox profile settings `XPROFILE_TITLE_SPECIFIC1/2/3` (0x63E83FFF/FE/FD).
   - Rank and grade are computed locally from the cR total and the `pggd` tag.
   - Purchases only set bits. **The balance is derived:** `balance = Rewards_GetDisplayCookieTotal() - Armory_GetCookiesSpent()`.
2. **[C] The server (LSP) is a reconciler.** Every 600 s, after a purchase, and on sign-in, the client POSTs a BLF file (`rpul` chunk) to `/gameapi_omaha/UserUpdateRewards.ashx?getDailyChallenges=1&userId=..&machineId=..`.
   - The reply is a BLF holding `rpdl` (the authoritative cR block, item flags and bonus grant), `dcha` (active daily and weekly challenges) and `fulc`.
3. **[C] If the server is unavailable**, the game keeps working "offline" on the profile copy.
   - It retries with backoff from 10 to 300 s.
   - After 30 s of failure it drops from "synced" to "offline".
   - Offline earnings are capped only if the profile was ever synced (default cap 1000 cR).
   - **No daily challenges** arrive without `dcha`.
4. Credit awards, rank thresholds, Armory prices and requirements, challenge rewards and commendation rewards are all **tag data** in every `.map` file: `gcrg`, `pggd`, `cpgd`, `chdg`, `comg`. Section 6 has tables.

## 1. Where the state lives at runtime

### 1.1 Rewards module ("persistent rewards")
- **[C]** Init: `Rewards_Initialize` @0x8258E3F0, called from 0x824D68A0.
  - It zeroes the module globals at 0x8390D070 (0x3CE0 bytes) and the sync globals at 0x83945D78 (0x7140 bytes).
  - It registers the callbacks: `0x8394CEB0 = Rewards_OnLspDownloadComplete` and `0x8394CEB4 = Rewards_OnLspRequestFailed`.
- **[C]** `0x83910D40` (u8) means "save and upload requested". It is set after an Armory purchase and consumed in 0x82224E78.
- **[C]** `0x83910D44` = Jenkins-style hash of `comg` (from `Rewards_HashCommendationGlobals` 0x825908F8). `0x83910D48` = hash of `cpgd` (from `Rewards_HashCookiePurchaseGlobals` 0x8221BF18).
  - Both hashes are stored with the profile block and sent to the server.
  - If they mismatch on load and the block is marked synced, the saved rewards are **ignored** (`Rewards_LoadFromProfile`). **Do not edit `cpgd` or `comg` tags without handling this.**
- **[C]** Per-controller state: `s_rewards_state` at `0x8390D078 + ctrl*0xF18`. The accessor is `Rewards_GetControllerState` 0x8258F738, which returns NULL unless the state byte is non-zero.

| Off | Type | Meaning |
|---|---|---|
| +0x000 | u8 | slot valid **[C]** |
| +0x001 | u8 | sync state: 0 = not loaded, 1 = loaded from profile / offline, 2 = synced with LSP **[C]** |
| +0x008 | block A, 0x208 | **current totals** (layout below) **[C]** |
| +0x210 | block B, 0x208 | delta earned while synced (online), since last upload **[C]** |
| +0x418 | block C, 0x108 | delta earned offline: {i32 cookies, i32 count, u8 itemflags[256]} **[C]** |
| +0x520 | i32 | unknown; persisted **[C]** |
| +0x524 | i32 + u16[256] | purchase log: count, then Armory entry indices **[C]** |
| +0x728 | u64 | last-modified time **[C]** |
| +0x734 | u32 | flags. bit0 = has synced with server (enables caps). bit1 = profile block valid / hashes present **[C]**, naming **[I]** |
| +0x768 / +0x770 / +0x778 | u64 | time of last profile save / last LSP download / server timestamp **[C]** |
| +0x780 / +0x781 / +0x782 / +0x783 | u8 | downloaded / offline cap hit / online cap hit / have server snapshot **[C]** |
| +0x788 | 0x760 | snapshot of +0x8..+0x768 merged with the server reply **[C]** |
| +0xEE8 / +0xEEC | i32 / u8 | pending bonus-credit grant from server: amount and type **[C]**; "loyalty/generic bonus" naming **[I]** |
| +0xEF4..+0xF10 | misc | copied from the profile block tail (xuid, times) **[C]** |

**Block A/B layout (0x208)** [C]:
- +0x000 i32 `cookies`. In A this is the lifetime total earned, which drives rank.
- +0x004 i32 `award count`.
- +0x008 a table of 32 entries × {i16 ×4}. These are per-source counters, saturated at 0x7FFF. Their meaning is **[I]**.
- +0x108 u8 `itemflags[256]`, indexed by `cpgd` entry.
  - bit0 = purchased/owned.
  - bit1 = visible/forced.
  - bit2 = prerequisite satisfied.
  - bit3 = granted free (not counted as spent).
  - bit4 = server-only.
  - The bits are confirmed by use; the names are **[I]**.

Delta math: `Rewards` 0x8258FCB8 computes a − b. 0x8258FB60 computes a + b, saturating, and copies the flags.

### 1.2 Player profile, persistence and setting IDs
- **[C]** Per-controller user slots sit at `0x839501D8 + ctrl*0xB88`. The profile struct is at slot+8, i.e. `0x839501E0 + ctrl*0xB88`. Profile fields:
  - flags u16 @+0: 0x1000 = not loaded / do not save, 0x800 = loaded OK, 0x400 = write pending, 0x4000 = write-failed toast shown.
  - **rewards block @+0x1E8 (0x548 bytes)**.
  - +0x1BB bit1, +0x1BC/+0x1BE: hide/pending bonus amounts (see 1.3).
  - +0xAE0 / +0xAE1: rank-up / grade-up pending flags, set by `Rewards_AddCookiesCheckRankUp`.
- **[C]** XAM calls:
  - `XamUserReadProfileSettings` (0x829EC8EC) is wrapped by 0x829700B0.
  - `XamUserWriteProfileSettings` (0x829ECB2C) is wrapped by 0x82970118.
  - The read task is 0x827111C8 (vtable 0x820628A8). It requests 17 settings from the table at 0x82020420: `0x63E83FFF, 0x63E83FFE, 0x63E83FFD` (TITLE_SPECIFIC1-3), then `0x10040003, 02, 04, 05, 0C, 0D, 0E, 15, 18, 1D, 1E, 22, 23, 24` (standard gamer settings).
  - The write task is 0x82711698 (vtable 0x820628D0).
- **[C]** Read path:
  - `Profile_OnReadSettingsComplete` 0x82711250 collects the 3 binary settings (type 6, source 2).
  - `Profile_DeserializeTitleSettings` 0x827120B8 requires total size 0xAD0 and version u32 0x27.
  - `Profile_ParseTitleBlob` 0x82712570 checks the SHA-1 and applies the fields; blob +0x1A0 goes to profile +0x1E8 via `Profile_SetRewardsBlock` 0x826F3EA0.
  - If XAM returns fewer than 14 settings, the profile gets flag 0x1000 and **the rewards never load**. The runtime XAM must return all requested IDs.
- **[C]** Write path: `Profile_SerializeTitleBlob` 0x82712EB0 builds the 0xAD0 blob, then `Profile_WriteTitleSettings` 0x82711E28 splits it:
  - TS1 = bytes [0, 1000)
  - TS2 = bytes [1000, 2000)
  - TS3 = bytes [2000, 2768)
- **[C] Blob integrity:** blob+0xAB8 holds a 20-byte SHA-1 of the whole 0xAD0 blob, computed with that field filled with 0x99 bytes.
  - It uses XeCryptSha via `Crypto_Sha1Buffer` 0x82214080 and 0x82206240(key = 0), so it is **unkeyed**.
  - 0x82206240 still requires XEX resource **"00"** (0x138 bytes, looked up via 0x821E5348). The recomp runtime must expose it, or every hash fails.
- **[C] Rewards block inside the blob** (blob +0x1A0, profile +0x1E8, 0x548 bytes). It is written by `Rewards_SaveToProfile` 0x8258F940 and read by `Rewards_LoadFromProfile` 0x8258F7F8.

| Off | Content |
|---|---|
| 0x000 | block A (0x208) |
| 0x208 | block B (0x208) |
| 0x410 | block C (0x108) |
| 0x518 | i32 (state +0x520) |
| 0x51C | i32 check = `~xuid_lo - cookies/10000` |
| 0x520 | u64 xuid |
| 0x528 / 0x52C | comg hash / cpgd hash |
| 0x530 | u64 save time |
| 0x538 | u64 = first 8 bytes of SHA-1("TIMM" ‖ i32 cookies ‖ u64 time) |
| 0x540 | u32 flags (state +0x734) |

- **[C] Other profile reads:**
  - Task 0x82708C80 reads TITLE_SPECIFIC1 of **title 0x4D5308CE** (MS-2254). The result feeds the Waypoint-unlock bits at `0x83960238 + ctrl*4`, used by `Armory_IsItemWaypointUnlocked` 0x8258FFE0 with `cpgd` +0xB4.
  - So 0x4D5308CE is most likely **Halo Waypoint** **[I]**.
  - DLC/promo unlock masks live at `0x8391F5EC + ctrl*0x28` (`Armory_IsItemDlcUnlocked` 0x8258EA88, `cpgd` +0xA8). Their source is **unknown**.

### 1.3 Balance, rank and grade
- **[C]** `Rewards_GetDisplayCookieTotal` 0x82110430 returns `max(0, A.cookies - (profile+0x1BB bit1 ? 0 : 5000) - max(0, profile+0x1BC) - max(0, profile+0x1BE))`.
  - **[I]** This hides a 5000 cR "initial credits" grant and server bonus grants until the UI shows the "awarded_*_credits" toast.
  - A local service should set bit1 of profile+0x1BB, or grant the 5000 cR.
- **[C]** `Armory_GetCookiesSpent` 0x82590580 = Σ `cpgd.entries[i].price` (+0x24) over items that are owned and not free.
- **[C]** `Rank_GetRankForCookies` 0x82110528, `Rank_GetGradeForCookies` 0x82110330 and `Rank_GetGradeDefinition` 0x82110608 walk the `pggd` tag (matg+0x670 ref, datum at matg+0x67C; `Rank_GetPlayerGradeGlobals` 0x82110740).
  - Ranks are capped by **netcfg** `0x82BD45A0` (max rank, default 20) and `0x82BD45A4` (max grade at max rank, default 4).
  - Thresholds can be overridden per [rank*5+grade] by netcfg `int[105] @0x82BD4754` (-1 = use tag).
- **[C]** Rank and grade go into the networked player configuration (`Player_BuildNetworkConfiguration` 0x821ECA28: rank at +0x57, grade at +0x2C). That is how other players see your rank.
- **[C]** `Achievements_Award(ctrl, id)` 0x8231FA10. IDs 36 and 31 are awarded on Armory purchase; 35 and 39 for challenges; 38 for commendations; 32 and 33 in 0x826F8AD8.

### 1.4 "Network configuration" (netcfg) globals
- **[C]** Block at 0x82BD28A8. Defaults are written by 0x82297140, called via 0x822970D0 when the LSP port-range bytes 0x82BD4130..0x82BD419C are all zero.
- **[I]** The block is the in-memory copy of `network_configuration_241.bin`. Path builder 0x820C9630 gives `/storage/title/4d53085b/default_hoppers/network_configuration_241.bin`. This is how Bungie retuned caps and rank limits remotely.

| Addr | Default | Use |
|---|---|---|
| 0x82BD45A0 | 20 | max rank index |
| 0x82BD45A4 | 4 | max grade at max rank |
| 0x82BD45A8 | 1000 | offline cR cap (block C) |
| 0x82BD45AC | 100000 | online cR cap (block B) |
| 0x82BD45B0 | f32[21*5] (f31, likely 1.0) | cR award multiplier per rank/grade |
| 0x82BD4754 | i32[21*5] = -1 | rank threshold overrides |
| 0x82BD4130/34 | port range | LSP ports (`Lsp_ResolveServerAddress`) |

## 2. Bungie.net / LSP protocol

### 2.1 Transport
- **[C]** LSP servers are reached with `XNetServerToInAddr(addr, service 0x4D530064)` in `Lsp_ResolveServerAddress` 0x82271E38.
  - It uses a random port in the netcfg range.
  - A debug override reads IP strings from 0x83251070 (stride 0xDA) via `inet_addr`.
- **[C]** HTTP/1.0 templates:
  - `GET %s HTTP/1.0\r\n%s\r\n` (0x82272730).
  - `POST %s HTTP/1.0\r\nContent-type: multipart/form-data; boundary=BUNGIEr0x0rz ...` (0x822722E0), with the part header `Content-Disposition: form-data; name="upload"; filename="%s"`.
- **[C]** Request descriptor (0x118 bytes, built by the `Lsp_Build*` functions):
  - `char url[256]`
  - +0x100 service class (0 = title storage, 1 = user storage, 2 = service record, 6 = rewards)
  - +0x104 needs-RSA-verify **[I]**
  - +0x108 timeout = 600
  - +0x110, +0x114 class flags
- **[C]** It is submitted with 0x82214208 into an HTTP request object (vtable 0x8206A268). The result is polled with 0x8221A918: 1 = running, 2 = done (data and size), else failed.
- **[C]** Responses are BLF chunk files: `_blf` header (0x30) … `_eof`. The validator is 0x822E19D8; `find_chunk(buf, size, 0, fourcc, version, &size, &payload, 0)` is 0x822E1CD8.
- **[C]** Web cache directories `cache1:\webcache` and `\dir_001` (0x82320D10…).

### 2.2 Rewards sync (the cR service)
`RewardsSync_UpdateController` 0x82605628 is a per-controller state machine. The struct is at `0x83945D80 + ctrl*0x1C40`:
- +0 state
- +0x10 request BLF (≤0x829 bytes)
- +0x86C upload size
- +0xD4C HTTP request object with a 0x859-byte response buffer
- `0x83945D79` = bitmask of controllers syncing

| Step | What | Evidence |
|---|---|---|
| trigger | `Rewards_SubmitToLsp` 0x8258EDD0 assembles the 0x778-byte `rpul` payload and calls `RewardsSync_BeginUpload` 0x82605288. Callers: state 1 in `Rewards_UpdateController`, every 600 s, and after a purchase via flag 0x83910D40. Gated by 0x821E76B0 on the profile ([I] Live-enabled check). | [C] |
| body | BLF: `_blf` (0x30, v1.2, BOM 0xFFFE); **`rpul` v3.1** (size 0x784); **`chpr` v2.1** (100 bytes; challenge progress from `Challenges_BuildProgressChunk` 0x821F5CA8); `loca` v1.1 (0x24 bytes from 0x82BD2692, meaning [I]); `_eof` (0x11). | [C] |
| URL | `/gameapi_omaha/UserUpdateRewards.ashx?getDailyChallenges=1&userId=%016I64x&machineId=%016I64x` (`Lsp_BuildUserUpdateRewardsRequest` 0x822DE658; service class 6). Content type `application/x-%s-reward-sync` (%s = string at 0x82BD2678). | [C] |
| reply | `rpdl` v2 (chunk size must be **0x227**) is passed to `Rewards_OnLspDownloadComplete` 0x8258F068. `dcha` v3 is passed to `Challenges_ApplyDchaChunk` 0x824BBF98. `fulc` v1 stores 3 ints at 0x8330E594..9C, which go into matchmaking session data (0x822D0420). | [C] |
| failure | Retry delay `0x8394CE80+ctrl*0xC` starts at 10 s (`RewardsSync_ResetRetryTimer` 0x82605210), doubles, and caps at 300 s. `Rewards_OnLspRequestFailed` 0x8258F6E8 demotes state 2→1 after ≥30000 ms of failure. A repeated-failure counter at 0x8330E698 can call 0x821EC938. | [C] |

**`rpul` payload (0x778)** [C]:
- 0x000 block A
- 0x208 block B
- 0x410 block C
- 0x518 purchase log (0x204)
- 0x71C i32
- 0x720 i32
- 0x724 comg hash
- 0x728 cpgd hash
- 0x72C u64 modified time
- 0x734 u8 flag
- 0x735 16 bytes (from 0x82271110, likely gamertag/identity [I])
- 0x745 u16
- 0x747 u64
- 0x74F u32
- 0x753 u8
- 0x754 u32
- 0x758 u32
- 0x75C u64
- 0x764 u64
- 0x76C u64
- 0x774 u32

**`rpdl` payload (0x21B)** [C]:
- 0x000 cookie block header (cookies, count, 32×4 i16)
- 0x108 itemflags[256]
- 0x208 u16
- 0x20A u32
- 0x20E u64
- 0x216 i32 bonus-credit amount
- 0x21A u8 bonus type

**Merge** [C] (`Rewards_OnLspDownloadComplete`):
- `new_A = local_A + (server - uploaded_A)`.
- Ownership merge: the bit is set if either side has it. Server-only items (cpgd flag 0x10, the "BNet sink" entries) take the server bit verbatim.
- If the server total is below the spent total, items are un-owned until spent ≤ total (anti-cheat).
- Then `Rewards_ApplyServerState` 0x8258FEE0: state = 2, flags bit0 set, profile pending bonus set, and the result is saved back to the profile.

### 2.3 Other Omaha/LSP endpoints seen
Status of each endpoint:
- `UserGetServiceRecord.ashx?machineId&shareId&userId`: requested by `Lsp_RequestUserServiceRecord` 0x826E9160 (service class 2) from the service-record UI. **[C]**
- `UserGetBnetSubscription`, `UserBeginConsume` / `UserCompleteConsume` (0x8270AA98 / 0x8270AB98), `ArenaUpdateGameResults` / `ArenaGetSeasonStats` (`arsr` v4, `arhs` v3), `MachineUpdateNetworkStats`, `SignBuffer`, the `Files*` (file share) endpoints, and `ReachPresenceApi`: string anchors only, not mapped further.
- User file: `/storage/user/4d53085b/%1X/%1X/%02X/%016I64X/user.bin` (`Lsp_BuildUserFileRequest` 0x82210550). It is registered in `OnlineFiles_RegisterUserFiles` 0x822DFC30. Its contents are **unknown**; it may carry the DLC/promo bits.

**[C]** The community "Reach Stats API" (`bungie.net/api/reach/...`) is **not** used by the game. The game only talks to `gameapi_omaha` and LSP storage.

## 3. Earning credits

| Function | Role |
|---|---|
| `Rewards_AwardCookies(ctrl, amount, kind)` 0x82590448 | Top-level award. Calls 0x825902F0, then adds to the per-controller per-kind u16 tally at `0x83151242+ctrl*0x11C` (game statistic "cookies", table 0x82A39E00 #19) and to the player object +0x230 (the "+N cR" HUD counter; +0x234 is its timer). [C] |
| `Rewards_AddCookiesCheckRankUp` 0x825902F0 | Wraps `Rewards_AddCookiesWithCaps` and sets profile +0xAE0 / +0xAE1 when rank or grade increases. [C] |
| `Rewards_AddCookiesWithCaps` 0x8258E770 | Adds to A.cookies and to B (state 2, cap netcfg 100000) or C (state 1, cap 1000). **Caps apply only if `state+0x734` bit0 is set**, i.e. the profile has ever synced. Over the cap it sets the toast flags at `0x8395ED9E/9F + ctrl*0x20`. A kind == 9 award bypasses the caps. No award when 0x821F96A8 returns flag 0x200 ([I] guest or temporary profile). [C] |

Award kinds [C]:
- 0 = commendation: per unit and per tier (`Commendation_IncrementProgress` 0x82590AA0; `comg` +0x8/+0xC per unit, +0x24 tier awards).
- 1 = challenge completed (`Challenges_OnChallengeCompleted` 0x824BC138; reward at entry +0x44).
- 5 = game-completion base.
- 6 = game-completion bonus (win or performance).
- 8 = conditional bonus from the game-options block at 0x8373FE98 [I: hopper/playlist bonus].
- 9 = uncapped.

**Post-game award** [C]:
- `GameResults_AwardGameCompletionCookies` 0x824C6350 loops over the 16 game players. For each one bound to a local controller it calls `Rewards_ComputeGameCompletionAward` 0x82590EE0.
- That function reads `gcrg` (matg+0x6B0, datum at +0x6BC): `top[param.cat].elem0.sub[param.sub] -> {i32 cR/min, f32 hopper, f32 winner, f32 performance, falloff{i16 minute, i16 cR/min}}`.
- `base = rankMult[rank][grade] * hopper * Σ(rate × minutes over the falloff curve)`; `total = base × (winner ? winner : performance-flag ? performance : 1)`.
- Winner comes from 0x824C5668. The performance flag means placing in the top half.
- It then awards kind 5 = base and kind 6 = total − base.
- The mapping of `cat`/`sub` to campaign, firefight, MP or custom is **[I]**. `gcrg` data shows 28 cR/min capped at 75 min (top 0), 28/min to 20 min (top 1, top 2), and custom games at 5/min to 10 min.
- The carnage-report UI reads these through game statistic "cookies" (table 0x82A39E00) **[I]**.

## 4. Daily and weekly challenges
- **[C]** Definitions are local (`chdg`, matg+0x6A0): categories bounties (16), weekly (38), campaign (55), firefight (55), multiplayer (56). Each challenge is 0x50 bytes, with the cR reward at +0x18.
- **[C]** The server only selects which challenges are active, via the `dcha` v3 chunk. Payload:
  - +0 u32 daily set id
  - +4 u32 weekly set id
  - +8 u64 daily expiry [I]
  - +0x10 u64 weekly expiry [I]
  - +0x18 u8 daily count
  - +0x19 u8 weekly count
  - +0x1A daily entries[≤10] × 0x1C
  - +0x132 weekly entries × 0x1C
  - Each entry starts with u8 a, u8 b. `a` is validated by 0x8258E2D0. **[I]** a = category, b = challenge index.
- **[C]** Runtime state is at 0x8373EE38:
  - daily set: id 0x8373EE40, expiry 0x8373EE48, count 0x8373EE50, entries of 0x9C from 0x8373EE54
  - weekly set at +0x630
  - initialized flag 0x8373FAA0
  - per-controller enable flag 0x8373FB10[ctrl]
  - progress per controller at stride 8 inside each entry
- **[C]** Progress is uploaded in `chpr`: daily id, weekly id, i32 progress[10] daily, i32 progress[10] weekly. `Challenges_LoadProgressChunk` 0x824E8CA8 restores it from a cached `chpr`.

## 5. Armory purchase flow
1. **[C]** UI: `UiArmory_OnItemSelected` 0x8278DF78. If the item is not owned:
   - `Armory_IsItemAvailable` 0x82110AD0 checks the requirements.
   - 0x82605D70 checks [I] "is purchasable in UI".
   - `Armory_CanAffordItem` 0x82606B40: `price ≤ DisplayTotal − Spent`.
   - If it can buy, it shows the confirm dialog (event 0x60061). Otherwise it shows message 0x4003C (requirements not met), 0x40023 or 0x40046.
2. **[C]** Confirm: `UiArmory_OnPurchaseConfirmed` 0x8278DDA0.
   - It calls `Armory_CommitPurchase(ctrl, entry)` 0x8258E8F8: sets owned bit0 in A, plus the same bit in B or C; logs the entry; marks modified.
   - It awards achievement 36 (31 if extra condition) and sets 0x83910D40 to request upload.
   - `Rewards_SaveAllToProfile` 0x8258E688 then saves the profile, and the item is equipped (0x82605EC0, `Profile` 0x826F3F30).
3. **[C]** Requirement checks:
   - `Armory_ArePurchaseRequirementsMet` 0x82606AC8 (entry+0x90) runs `Armory_CheckRankRequirement` 0x826067F8 ({u8 rank, i8 grade} list) and `Armory_FindMissingPrerequisite` 0x826068A8 (entry+0x9C list).
   - Ownership: `Armory_IsItemOwned` 0x82110A50 = owned bit OR free.
   - Free: `Armory_IsItemGrantedFree` 0x8258EA08 = bit3, DLC (0x8258EA88) or Waypoint (0x8258FFE0).
4. **[C]** Housekeeping:
   - `Armory_AutoBuyFreeItems` 0x82606718 buys auto-flagged or 0-cost items once their requirements are met.
   - `Armory_ValidateOwnedItems` 0x82590150 drops owned bits whose visibility requirements fail.
5. **[C]** Data: `cpgd` `ui\supply_depot` (reached through `Armory_GetCookiePurchaseGlobals` 0x82110790 → UI-globals tag chosen by game mode, +0x78).
   - It has 200 entries of 0xBC bytes: price +0x24, flags +0xC, visibility rank +0x64, buy rank +0x90, prereqs +0x9C, DLC +0xA8 / +0xAC, Waypoint list at header +0xB4.
   - Entries 169–199 are "BNet sink" items priced 2^n: server-side debits [I].

## 6. Tag data (from the maps)
The map parser lives in the session scratchpad, not the repo. Values are confirmed from file bytes; field names come from the Assembly and TagTool plugins.

| Group | Name | Path (mainmenu.map) | Reached from |
|---|---|---|---|
| `pggd` | player_grade_globals_definition | `globals\player_grade` | matg+0x670 (code: +0x67C) |
| `cpgd` | cookie_purchase_globals | `ui\supply_depot` | wgtz `ui\main_menu` +0x6C (code: +0x78) |
| `gcrg` | game_completion_rewards_globals | `globals\game_results` | matg+0x6B0 (code: +0x6BC) |
| `chdg` | challenge_globals_definition | `globals\challenges` | matg+0x6A0 |
| `comg` | commendation_globals_definition | `globals\commendations` | igpd+0x2C (code: 0x82227A00) |
| `igpd`, `pmcg`, `achi`, `avat` | incidents, model customization, achievements, avatar | `globals\...` | matg+0x660/0x680/0x6C0/0x6D0 |

The same data is in every map (checked mainmenu, 20_sword_slayer, m10 and ff10). String IDs differ per map, so saves should use indices.

**Rank table (`pggd`, cR needed; ranks 0..20 × grades)** [C]:

| Rank | Grades (cR needed) |
|---|---|
| Recruit | 0; Private 7,500 |
| Corporal | 10,000; 15,000 |
| Sergeant | 20,000; 26,250; 32,500 |
| Warrant Officer | 45,000; 78,000; 111,000; 144,000 |
| Captain | 210,000; 233,000; 256,000; 279,000 |
| Major | 325,000; 350,000; 375,000; 400,000 |
| Lt. Colonel | 450,000; 480,000; 510,000; 540,000 |
| Commander | 600,000; 650,000; 700,000; 750,000 |
| Colonel | 850,000; 960,000; 1,070,000; 1,180,000 |
| Brigadier | 1,400,000; 1,520,000; 1,640,000; 1,760,000 |
| General | 2,000,000; 2,200,000; 2,350,000; 2,500,000; 2,650,000 |

Ranks 11–20 (one grade each):

| Rank | cR needed |
|---|---|
| Field Marshall | 3,000,000 |
| Hero | 3,700,000 |
| Legend | 4,600,000 |
| Mythic | 5,650,000 |
| Noble | 7,000,000 |
| Eclipse | 8,500,000 |
| Nova | 11,000,000 |
| Forerunner | 13,000,000 |
| Reclaimer | 16,500,000 |
| Inheritor | 20,000,000 |

Commendations (`comg`, 45 entries of 0x34): for example headshot_mp has tiers 50/250/1000/4000/8000/20000, tier awards of 150/425/1200/2400/2400/0 cR, and 7 cR per unit.

## 7. Unknowns and next steps
1. How netcfg is applied: confirm that the downloaded `network_configuration_241.bin` overwrites 0x82BD28A8, find its parser, and record the 1.0 launch rank cap Bungie served.
2. `gcrg` `cat`/`sub` selection inside 0x824C6350 (the `uStack_88` packing). Award kinds 2, 3, 4 and 7, and the callers of kind 8 and 9.
3. Meaning of the 32×4 i16 counters in blocks A/B, of rpdl +0x208..+0x215, of rpul +0x735..+0x777, and of `fulc` and `loca`.
4. The `dcha` entry bytes beyond [0..1], and the expiry units (FILETIME?).
5. Source of the DLC/promo mask at 0x8391F5EC (user.bin? content enumeration? avatar awards?). Contents of user.bin.
6. Whether any UI gates the Armory or credits on being online or synced. The code paths seen work in state 1.
7. Where the 5000 cR "initial credits" and profile+0x1BB bit1 come from (rpdl bonus? UI acknowledgement?).
8. Confirm 0x4D5308CE = Halo Waypoint.

## 8. Recommendation: where a local credits/Armory service should hook

**Phase 1 (no server, works today).** Purchases, rank, cR earning and the Armory are all client-side, so just make persistence work:
- (a) The ReXGlue XAM layer must persist `TITLE_SPECIFIC1/2/3` (0x63E83FFF/FE/FD) per XUID, return all 17 requested settings with source = title for stored ones, and accept writes.
- (b) XEX resource "00" must be readable, for the SHA-1 helper 0x82206240.
- (c) A never-synced profile has `+0x734` bit0 clear, so there are **no caps**.
- (d) Set profile+0x1BB bit1, or grant 5000 cR, so the display total isn't reduced.
- Optional: patch netcfg 0x82BD45A8/AC after 0x82297140 if caps ever appear.

**Phase 2 (local "Omaha" service)** for daily/weekly challenges, server-side reconciliation, bonus grants and multi-device sync:
- Intercept the HTTP layer rather than individual game functions. Hook the request submit at 0x82214208 and the result poll at 0x8221A918, or `Lsp_ResolveServerAddress` 0x82271E38 to point at localhost.
- Answer `/gameapi_omaha/UserUpdateRewards.ashx`:
  - parse `rpul` (blocks A/B/C, purchase log, hashes) and `chpr`;
  - keep authoritative totals per XUID (add B and C deltas, validate purchases against `cpgd` prices and `pggd` ranks);
  - reply with BLF `_blf` + `rpdl` v2 (0x227) + `dcha` v3 (daily and weekly picks from `chdg`, with rotating ids and expiries) + `_eof`.
- The game's own merge code (`Rewards_OnLspDownloadComplete`) then does the rest.
- Never set the BNet-sink item bits (169–199) except to debit deliberately.
- Fallback if HTTP emulation is too heavy: call `Challenges_ApplyDchaChunk` 0x824BBF98 directly with a locally built `dcha` payload, and leave rewards in profile-only mode.
