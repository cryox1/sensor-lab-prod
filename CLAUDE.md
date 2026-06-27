# CLAUDE.md

@AGENTS.md

The rules in AGENTS.md are **mandatory**. The production TimescaleDB was wiped once
by a deploy that overwrote the server's compose and `rsync --delete`'d the tree —
every rule there exists to prevent a repeat. Before any deploy or DB-touching action,
back up first (`./deploy/backup.sh`) and use only `./deploy/deploy.sh`.
