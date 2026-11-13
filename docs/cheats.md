# Built-in cheats

The settings menu's Cheats tab offers built-in policies on game 1.1.1, identified by the main build ID
`E0F3CCFAF50065AE`. Other builds keep the existing imported-cheat interface without these built-ins. Every
toggle defaults to off; sprint speed, spin range and spin charging default to normal. Settings persist in
`nx2navi.ini`. NX2navi 0.9.2 is the port release used for the spin gameplay measurements below; the supported
game version remains 1.1.1. The contribution is also integrated with the subsequent upstream main changes.

| Group | Options |
|---|---|
| Link | Infinite health, magic and rupees; unlimited air jumps; sprint speed normal / 2× / 3.5× / 5×; spin attack range normal / 1.5× / 2× / 2.5×; magic spin charging normal / instant. |
| Inventory | Separate infinite magic beans and business seeds; contents of each unlocked bottle, up to five. |
| Ammunition | Separate infinite arrows, bombs, bombchus, Deku nuts, slingshot seeds and Deku sticks. |
| Timers | Freeze the supported shared countdown at its current remaining time. |

Resource policies preserve existing capacities and unlocks. Health does not revive a dead player. Magic requires
an existing magic meter; ammunition requires its existing capacity, with an additional obtained-item check for
bombchus. Magic beans and business seeds require their obtained flags and use native limits of 10 and 30.

Bottle rows appear only for unlocked slots. Each row selects empty, red/green/blue potion, blue fire, fish,
letter, Big Poe, Poe, bugs, fairy, milk or half milk. An edit updates the current and buffered contents once;
normal consumption continues afterward. Save in the game to retain the contents. Bottle validity and stock
are never changed.

Unlimited Jumps responds to another press of the configured jump action. Holding the button does not repeat
jumps. Sprint Speed scales the native sprint action without changing ordinary movement, gravity or global
game speed. Lower speeds can suit confined locations better. Text refers to actions rather than physical
controller buttons. Labels and help follow the selected menu language: English, Russian, Brazilian Portuguese
or Simplified Chinese.

Spin Attack Range multiplies the native collision geometry for the equipped sword's spin attack. Each sword
keeps its own base reach. The game controls the attack window, updates its physics shape and restores ordinary
sword geometry afterward. The option changes hit reach; the sword model and visual effects do not grow with it.
It does not scale ordinary sword attacks or other actors' weapons. A changed range takes effect when the next
native spin shape is calculated. `spin_range` accepts `normal`, `1.5`, `2` or `2.5`.

Magic Spin Charge selects `normal` or `instant` through `spin_charge`. Instant removes the additional wait for
the fully charged magic spin, reaching it on the next gameplay update. Drawing the sword and the normal attack
animation still run. The Great Fairy upgrade and native magic checks remain required. Range and charge speed
are independent options; neither edits the unlock flag or a save directly.

Freeze timers acts only while the shared timer is running in countdown mode with a positive finite value.
Disabling it resumes normal counting. Count-up clocks, bomb fuses and unrelated temporary effects are not
modified. The shared clock was verified live in the Zora diving minigame on a local 0.7.1 Windows build;
references in other event scripts establish reuse of the interface, not testing of every event or its other
failure conditions.

## Implementation and checks

`runtime/cheats.c` validates the build, table shapes, key hashes, indices, readable memory and relevant object
identities before writing. Health and magic capture their live resource bindings through independent bits of
the existing function-entry observer; the resolution observer keeps its own bit. The sprint patch is declared
in `recomp/patches-v111.txt`: the generator validates the original instruction and emits the optional multiplier
path. Spin range uses three native spin-only result sites and a host callback registered in `runtime/patches.c`.
The callback validates Link's current sword and its reference generation, then multiplies the native length and
thickness scales in floating-point return registers. There is no cached collision shape to restore across a
weapon change. Instant changes only the native second-phase charge threshold to 0.125 ticks, a positive value
that avoids division by zero. Original words at all patch sites are validated by the generator. Generated C is
not edited by hand or checked in.

`python tests/cheats_check.py` compiles the real policies against synthetic guest memory, without game files.
Set `CC` to the UCRT64 clang executable when it is not on PATH. The test covers the build gate, health/death
and capacity guards, magic capacity, wallet limits, item ownership, legacy no-op policies, all 13 contents in
each of five bottles, locked-slot and invalid-enum rejection, countdown freezing, count-up exclusion and expired
or non-finite timers, all four spin multipliers, preservation of other CPU registers, and stale/non-sword guards.
`--probe-ui-text` checks the language catalogs and live menu-language selection. Changes
under `recomp/` also require the differential checks in [building.md](building.md).

On a 0.9.2 Windows build, isolated save-copy runs measured the Kokiri sword's fully charged spin capsule length
as 1.224 normally and 3.06 at 2.5×, with radius 1.5 and 3.75. The partial charge measured 0.304 / 1.35 normally
and 0.76 / 3.375 at 2.5×. Both returned to ordinary geometry 0.8 / 0.15 after each attack. The second charge phase
took about 1.5 seconds normally and no more than one gameplay update with Instant (sampling about every 53 ms).
All 70 original save, backup and transaction files were unchanged by hash comparison. These are geometry and
timing checks, not an enemy-hit or every-sword combat test. Other swords share the native calculation but were
not individually exercised; a separate locked-upgrade save was not tested live. Other events' timer branches,
Linux and other game builds remain unvalidated.
