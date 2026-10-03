# What we took from SkyCraft

SkyCraft (chasm, github.com/chasmlol/SkyCraft, MIT) runs Minecraft as a hidden guest inside
Skyrim: you play Skyrim as a Minecraft player, with Minecraft's physics, blocks and combat
against Skyrim's NPCs. Version 0.1.0 was read on 2026-10-02 from
`C:\Users\Uzann\Downloads\SkyCraft-0.1.0`. It targets **Skyrim AE 1.7.104, the same build as
ours**, so its Address Library IDs and structure offsets work for us directly. Reusing its
code is fine under MIT with attribution; `src/skse_ghosts/havok_export.cpp` credits it.

## Same architecture, different guest

Both projects use the same idea:
- the host (Skyrim) sends its camera;
- the guest renders with it;
- the image is composited by depth in a ReShade add-on;
- the host's collision goes back to the guest.

The differences that make our side harder:

| | SkyCraft (Minecraft) | Us (DDDA) |
|---|---|---|
| Modding | Fabric, Mixins, unobfuscated Java | closed 32-bit exe, everything reverse-engineered |
| Host collision in the guest | extra AABBs appended to MC's collision query at runtime | SBC tile archives written to disk, read by DDDA when a tile loads |
| Navigation | MC mobs path on the same blocks | separate waypoint graphs (WAY) that we generate |
| Guest characters | one player, driven by input | three AI pawns that decide where to go |
| Stand-in creation | `new Entity` + `addFreshEntity` | DDDA enemy spawning still has to be found |

## Collision (adopted, 2026-10-02)

Source: `skse/src/Collision.cpp`. The idea is to read Skyrim's **live Havok world** around
the player instead of rebuilding collision offline from Skyrim.esm and the meshes.

- Take the player cell's `GetbhkWorld()`, then `GetWorld1()`, then the islands (`fixedIsland`,
  active and inactive), then each entity's `collidable`. Filter by collision layer, read
  `shape` and `motion` (the transform), and walk the shapes under the world read lock.
- Shape walking:
  - MOPP and BV trees are queried with the region box (moved into shape space) through
    `QueryAabbImpl` plus the container's `GetChildShape`.
  - Lists, collections, compressed meshes and convex lists are walked key by key.
  - Triangle vertices are at +0x30/+0x40/+0x50.
  - A box has half extents at +0x30 and a radius at +0x20.
  - A capsule has its points at +0x30/+0x40.
  - Convex vertices keep their planes in an `hkArray<hkVector4>` at +0x78.
  - Convex transform and translate shapes have the child at +0x30 and the transform at +0x40.
  - A transform shape has the child at +0x28 and the transform at +0x50.
  - Every read runs under SEH, because the layouts are partly reverse-engineered.
- A Havok transform holds its rotation columns at [0..2], [4..6], [8..10] and its translation
  at [12..14]. `bhkWorld::GetWorldScaleInverse()` converts Havok units to game units.
- Collision layers we keep: static (1), anim static (2), transparent (3), trees (9),
  terrain (13, rock and cliff pieces), ground (17, which is the **landscape** itself) and
  stair helper (31, Skyrim's invisible stair ramps). We leave out props, clutter, actors,
  water, triggers and invisible walls.
- Our version (`havok_export.cpp`) harvests each attached exterior cell **once per session,
  per cell**, into files. SkyCraft instead streams 8-block regions around the player and
  re-sends the near ones every second (doors).
- Their steep-surface trick: cells steeper than ~50 degrees become full wall columns, so MC's
  step-up rule refuses cliffs.
- What we added on top for DDDA:
  - invisible ramps at 15-70 cm lips, because DDDA steps up less than Skyrim;
  - "reachable on foot" floors for navigation;
  - waypoint nodes snapped to the middle of tight passages.

  All of this is in `docs/terrain-proxy.md`, "Live Havok collision".

## Combat (not done yet: the plan for the pawns)

Sources: `skse/src/Combat.cpp`, `fabric/.../combat/SkyrimActorEntity.java`, `SkyCombat.java`.

### Skyrim NPC to guest: stand-ins

For every Skyrim actor within ~80 blocks, the guest spawns an **invisible, passive stand-in**:
no physics, no AI, placed every tick at the actor's position, sized from its height and bound
radius, with its health never dropping. The guest's own combat code hits it, and the damage
is summed per tick and sent to Skyrim. For us this means a DDDA enemy object at the NPC's
position, which the pawns attack with DDDA combat. Finding how to spawn and drive one is the
open problem.

### Damage into Skyrim through its own hit pipeline

Building the hit this way gives stagger, blood, pain sounds, aggro, crime and kill credit:
1. Construct a HitData (constructor ID 43995).
2. Call `Populate(attacker, victim, nullptr)`.
3. Set the weapon (a vanilla stand-in weapon, for impact sounds and blood), the hit position
   and direction, the damage fields, stagger, push-back and the skill.
4. Call ProcessHit, ID 38586. Its address is validated first: the call at ID 38627 + 0x4A8
   must be `E8` and point to it.

Fallbacks:
- `DoDamage` plus the `staggerDirection` / `staggerMagnitude` graph variables and
  `staggerStart`;
- `BGSImpactManager::PlayImpactEffect` for the blood spray;
- `StartCombat` so that the NPC fights back.

### Skyrim NPC to the player (for us: to a pawn's Skyrim "ghost")

SkyCraft does not cancel hits on the Skyrim player. It lets Skyrim apply them and then
**refunds** the health:
- the deficit (max minus current) is restored with `RestoreActorValue` and sent to the
  guest;
- a `TESHitEvent` sink records the kind of hit (projectile, magic by source form type,
  power attack, blocked);
- a hook on PlayerCharacter vfunc 0x104 (`HandleHealthDamage`) records the attacker;
- `kEssential` is set so that the Skyrim body never dies on its own;
- magic and damage over time are batched every 0.5 s.

For the pawns, the same thing would run on an invisible Skyrim actor ("ghost", docs/ghosts.md)
standing at each pawn, with the damage written to the pawn's HP in DDDA (`[char+4BC]+1D8`).

### Damage scaling

- Guest to Skyrim: × (5 + 0.25 × NPC level).
- Skyrim to guest: ÷ 5.

Both are config values. They are a starting point for calibrating DDDA against Skyrim.

### Other pieces

- **Arrows**: the guest flies the projectile. When it sticks in a stand-in, Skyrim attaches
  an arrow to the NPC's skeleton at the hit point. Our pawns' arrows and spells are not
  drawn yet: isolate.cpp keeps only the party's meshes.
- **Explosions**: `AIProcess::KnockExplosion` on actors in range, plus linear impulses on the
  ragdoll and on loose physics bodies (`TraverseScenegraphCollision`, under the bhkWorld
  write lock).
- **Hazards**: lava and fire under an NPC deal damage, cast HazardFireSpell (0x153BD) and
  make it scramble out.

## Pathfinding around the guest's things

Source: `skse/src/PathAvoid.cpp`. Skyrim builds an NPC's path request in one function,
AE ID 41642 (actor, request, goal, radius, avoid nodes). SkyCraft hooks its 7 call sites
(37778+0xDB, 37819+0x7D, 37820+0xCE, 41608+0x1E1, 41610+0xC5, 41611+0xE9, 41641+0x58).

The hook adds `BSPathingAvoidNode` cylinders (radius 0.75 block, cost 1000) for the guest's
solid columns near the route. The avoid array is a ref-counted `BSTArray` (0x20 bytes),
allocated with `RE::malloc`.

This could keep Skyrim ghosts or NPCs from walking through things that exist only in DDDA.

## Practices worth copying

- Test stand-ins for each side (`tools/fake_skyrim.py`, `fake_guest.py`). We have
  `terrain_sim.py` and `light_driver.py`.
- Validate every hardcoded call target before using it: check the `E8` and the target, and
  fall back if they do not match.
- A layout header shared by both sides, with static asserts (our `bridge_shared.h`).
