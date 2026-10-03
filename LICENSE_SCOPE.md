# Licence scope

What Hexlith's licence covers, and what no licence here could cover. This file describes scope. It is not a licence for Nintendo's software or for anybody else's.

## Original contributions

Code and documentation written for this project are licensed under the GNU General Public License, version 3 or, at your option, any later version, with the identifier `GPL-3.0-or-later`. The full text is in `LICENSE`.

## Third-party components

Components taken from other projects keep their own licences, copyright headers and notices. Adding a licence at the root of this repository does not relicense anything received from elsewhere.

Hexlith's expected donors are Azahar, licensed `GPL-2.0-or-later`, and TriAevum, licensed `GPL-3.0-or-later`. The "or later" option in the first permits a combined work to be conveyed under GPL-3.0-or-later. Permissively licensed components keep their MIT or BSD notices.

Attribution is preserved through source file headers, the component licence texts, and `THIRD_PARTY_NOTICES.md`. Those notices have to travel with source and binary distributions as the applicable licences require. That is not an additional restriction on the freedoms the GPL grants.

## Excluded material

No permission is granted here, and none could be, for:

- Nintendo ROMs, executable code, assets, firmware, encryption keys, trademarks or any other proprietary material
- content extracted from a cartridge image, including code, modules, textures, models, audio and video
- generated or lifted code derived from the game, and decompiled game source
- proprietary SDKs and redistributable libraries whose own terms do not permit inclusion

None of that is present in this repository, and `ADR 0007` describes the three layers that keep it that way.

A user's own extraction, carried out locally from a cartridge they own, is their own data. Whether a particular extraction or use is lawful depends on where they are and what they own, and nothing in this repository determines that.

## What the technical separation does and does not mean

The guard scripts and the CI checks reduce the chance of accidental inclusion. They are not a legal opinion, and they do not make any particular use of this project lawful.
