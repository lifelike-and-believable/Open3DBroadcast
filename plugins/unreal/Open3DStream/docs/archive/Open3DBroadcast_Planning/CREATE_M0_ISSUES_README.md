# Creating GitHub Issues for Open3DBroadcast M0 Milestone

This directory contains issue documentation and a script to create GitHub issues for the M0 (Protocol and Role Alignment) milestone.

## Issue Documentation Files

- **ISSUE_M0_1_PROTOCOL_ALIGNMENT.md** - Protocol message types and versioning confirmation
- **ISSUE_M0_2_CURVE_SEMANTICS.md** - Curve semantics and filtering rules  
- **ISSUE_M0_3_TRANSFORM_SPACE.md** - Transform space, coordinate system, and timing decisions
- **ISSUE_M0_4_TRANSPORT_ROLES.md** - Transport role pairing documentation
- **ISSUE_M0_INDEX.md** - Quick reference index for all M0 issues

## Creating GitHub Issues

### Prerequisites

- GitHub CLI (`gh`) installed and authenticated
- Repository access to create issues

### Quick Start

Run the script to create all 4 M0 issues:

```bash
./create_m0_github_issues.sh
```

The script will:
1. Create 4 GitHub issues with appropriate titles and labels
2. Link each issue to its full documentation file
3. Include key deliverables, references, and dependencies
4. Apply milestone and area labels

### Manual Issue Creation

If you prefer to create issues manually or customize them:

1. Go to https://github.com/lifelike-and-believable/Open3DStream/issues/new
2. Use the content from the issue documentation files
3. Apply labels as suggested in each file (see the "Labels" section at the bottom)
4. Link to the documentation file in the issue body

### Issue Structure

Each created issue includes:

- **Title**: `[M0.X] Issue Title`
- **Body**: 
  - Link to full documentation file
  - Context and purpose
  - Key deliverables checklist
  - Estimated effort
  - Dependencies
  - Priority level
  - Key references with links
  - Next steps
- **Labels**: `milestone:M0`, area labels, priority labels

### Viewing Created Issues

After running the script, view all M0 issues:

```bash
gh issue list --repo lifelike-and-believable/Open3DStream --label milestone:M0
```

Or visit: https://github.com/lifelike-and-believable/Open3DStream/issues?q=label%3Amilestone%3AM0

## Issue Labels

The script applies these labels to organize issues:

- `milestone:M0` - M0 milestone marker
- `area:protocol` - Protocol-related work
- `area:documentation` - Documentation tasks
- `area:curves` - Curve-related work
- `area:transforms` - Transform-related work
- `area:timing` - Timing-related work
- `area:transport` - Transport/networking work
- `area:networking` - Network protocols
- `critical` - Critical priority (M0.3)
- `good-first-task` - Good for new contributors (M0.1, M0.4)
- `foundation` - Foundation work (M0.1)

**Note**: Some labels may need to be created in the repository before the script runs successfully.

## Implementation Order

Recommended sequence:

```
M0.1 (Protocol) → Foundation (1-2 days)
    ├─→ M0.2 (Curves) - Can run in parallel
    ├─→ M0.3 (Transforms) - Can run in parallel [Critical priority]
    └─→ M0.4 (Transports) - Can run in parallel
```

Total estimated effort: 4-5 days with parallelization, or 6-10 days sequential.

## Documentation

- **Master Plan**: `plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md`
- **Issue Index**: `ISSUE_M0_INDEX.md`
- **Individual Issues**: `ISSUE_M0_*.md` files

## Troubleshooting

### Script fails with "label not found"

Create missing labels in the repository:
```bash
gh label create "milestone:M0" --repo lifelike-and-believable/Open3DStream
gh label create "area:protocol" --repo lifelike-and-believable/Open3DStream
# ... etc for other labels
```

### Authentication error

Authenticate with GitHub CLI:
```bash
gh auth login
```

### Permission denied

Ensure you have write access to the repository and the script is executable:
```bash
chmod +x create_m0_github_issues.sh
```

## Next Steps

After M0 issues are complete:
- **M1**: Single-mesh capture (uses M0 protocol/transform/curve specs)
- **M2**: Serialization integration (uses M0 protocol understanding)
- **M3**: Transport integration (uses M0 transport pairing guide)
