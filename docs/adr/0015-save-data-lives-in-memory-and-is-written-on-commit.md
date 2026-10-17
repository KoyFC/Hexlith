# 0015. Save data lives in memory and is written on commit

Status: Accepted
Date: 2026-10-17

## Context

On a first boot the title finds its save data unformatted, which is what sends it to language selection. It then formats the archive, creates its files, writes them and asks fs:USER to commit, and on the way to the title screen it also creates an extra data archive on the SD card and fills a few directories in it. A later boot reads all of that back and skips language selection.

Two needs pull against each other. A boot should be able to resume from what an earlier one saved, which means the archive outlives the process, in the workspace. And the boots that are compared with Azahar, or with each other for determinism, have to start from the same state every time, which a file left behind by the run before quietly breaks.

The console itself writes save data through a journal: what the title writes is not durable until it sends ControlArchive with the commit action. Azahar writes every change to the host at once and answers the commit with success.

## Options

**Write through to the disk, as Azahar does.** Simple, and every run changes the workspace as it goes, including a run that is meant to be reproducible. A crash leaves a half-written archive.

**Keep the archive in memory and write it out on commit and when the process ends.** The domain holds the tree and knows nothing of disks. The application decides where, and whether, it is written.

**Keep it in memory and never write it.** Reproducible, and a second boot can never see what the first one saved.

## Decision

The second. `services::SaveData` is a tree of directories and files held in memory, with the rules of the console's archive: the results Azahar's save data archive gives for each case, and for extra data the fixed-size files Azahar's extra data archive enforces. The FS service hands every commit to a function the application layer supplies, and the application writes the archive out then and when the run ends.

On disk it is a directory per archive under `work/<id>/save/`: `savedata/` for the title's save data and `extdata/<media>-<high>-<low>/` for each extra data archive, each holding a `format.txt` with what the archive was formatted with and a `files/` tree that mirrors the archive's own, empty directories included. A write goes whole to a sibling directory first and replaces the old one by renaming, so a run that stops halfway never leaves half an archive where the next run reads one.

A run started as a new game neither reads nor writes any of it: the archive starts unformatted and stays in memory.

## Consequences

A run is reproducible by construction when it starts as a new game, and resumable when it does not.

Nothing the title writes reaches the disk until it commits or the run ends. A title that wrote save data and was killed before either would lose it here and keep it under Azahar. The reference title commits each time it saves, so this has not been seen to matter.

The archive is written whole on each commit. The reference title's save data is a few hundred kilobytes, so this costs nothing measurable; a title with large save data might want a journal of its own.
