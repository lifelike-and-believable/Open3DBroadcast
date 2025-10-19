#!/bin/bash
# Script to create GitHub issues for Open3DBroadcast M0 milestone
# This script creates 4 issues from the ISSUE_M0_*.md documentation files
# Usage: ./create_m0_github_issues.sh

set -e

REPO="lifelike-and-believable/Open3DStream"
MILESTONE="M0"

echo "Creating GitHub issues for Open3DBroadcast M0 milestone..."
echo "Repository: $REPO"
echo ""

# Get the base URL for linking to documentation
BRANCH=$(git rev-parse --abbrev-ref HEAD)
BASE_URL="https://github.com/$REPO/blob/$BRANCH"

# Issue M0.1 - Protocol Message Types and Versioning Confirmation
echo "Creating Issue M0.1 - Protocol Message Types and Versioning Confirmation..."
gh issue create \
  --repo "$REPO" \
  --title "[M0.1] Protocol Message Types and Versioning Confirmation" \
  --label "milestone:M0,area:protocol,area:documentation,good-first-task,foundation" \
  --body "## 📋 Issue Documentation
[**Full Issue Specification: ISSUE_M0_1_PROTOCOL_ALIGNMENT.md**]($BASE_URL/ISSUE_M0_1_PROTOCOL_ALIGNMENT.md)

## Context and Purpose

Before implementing Open3DBroadcast, we must confirm the exact message types, versioning strategy, and serialization APIs from the existing O3DS protocol. This ensures the broadcast module uses the protocol correctly and maintains compatibility with existing receivers (Unreal LiveLink Source, Maya, MotionBuilder).

## Key Deliverables

- [ ] Protocol reference document created
- [ ] FlatBuffers schema documented for broadcast use case
- [ ] Serialization API usage documented
- [ ] Validation against existing receivers complete

## Estimated Effort

1-2 days

## Dependencies

None (foundation issue)

## Priority

**High** - Foundation for all broadcast work

## Key References

- **Full Issue Spec**: [ISSUE_M0_1_PROTOCOL_ALIGNMENT.md]($BASE_URL/ISSUE_M0_1_PROTOCOL_ALIGNMENT.md)
- **Master Plan**: [Open3DBroadcast_Planning.md]($BASE_URL/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md)
- **M0 Index**: [ISSUE_M0_INDEX.md]($BASE_URL/ISSUE_M0_INDEX.md)
- Protocol schema: \`src/o3ds.fbs\`
- Model API: \`src/o3ds/model.h\`, \`src/o3ds/model.cpp\`
- [CURVE_SUPPORT.md]($BASE_URL/CURVE_SUPPORT.md)

## Next Steps

Once complete, enables:
- M0.2: Curve Semantics
- M0.3: Transform Space & Timing
- M0.4: Transport Role Pairing"

echo "✓ Issue M0.1 created"
echo ""

# Issue M0.2 - Curve Semantics and Filtering Rules
echo "Creating Issue M0.2 - Curve Semantics and Filtering Rules..."
gh issue create \
  --repo "$REPO" \
  --title "[M0.2] Curve Semantics and Filtering Rules" \
  --label "milestone:M0,area:curves,area:protocol,area:documentation" \
  --body "## 📋 Issue Documentation
[**Full Issue Specification: ISSUE_M0_2_CURVE_SEMANTICS.md**]($BASE_URL/ISSUE_M0_2_CURVE_SEMANTICS.md)

## Context and Purpose

Define curve naming conventions, value ranges, and filtering rules based on \`CURVE_SUPPORT.md\`. Specifies how to capture morph targets and animation curves from Unreal and map them to protocol format.

## Key Deliverables

- [ ] Curve capture reference document created
- [ ] Naming convention mapping defined (Unreal → protocol)
- [ ] Value range specifications documented
- [ ] Filtering rules specified

## Estimated Effort

1-2 days

## Dependencies

- **Requires**: Issue M0.1 (Protocol Message Types) to be completed first

## Priority

**High**

## Key References

- **Full Issue Spec**: [ISSUE_M0_2_CURVE_SEMANTICS.md]($BASE_URL/ISSUE_M0_2_CURVE_SEMANTICS.md)
- **Master Plan**: [Open3DBroadcast_Planning.md]($BASE_URL/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md)
- **M0 Index**: [ISSUE_M0_INDEX.md]($BASE_URL/ISSUE_M0_INDEX.md)
- [CURVE_SUPPORT.md]($BASE_URL/CURVE_SUPPORT.md)
- Test files: \`test_curve_comprehensive.cpp\`, \`test_curves.cpp\`

## Next Steps

Once complete, enables:
- M1: Single-mesh curve capture implementation"

echo "✓ Issue M0.2 created"
echo ""

# Issue M0.3 - Transform Space, Coordinate System, and Timing Decisions
echo "Creating Issue M0.3 - Transform Space, Coordinate System, and Timing Decisions..."
gh issue create \
  --repo "$REPO" \
  --title "[M0.3] Transform Space, Coordinate System, and Timing Decisions" \
  --label "milestone:M0,area:transforms,area:timing,area:protocol,area:documentation,critical" \
  --body "## 📋 Issue Documentation
[**Full Issue Specification: ISSUE_M0_3_TRANSFORM_SPACE.md**]($BASE_URL/ISSUE_M0_3_TRANSFORM_SPACE.md)

## Context and Purpose

Make critical decisions about transform space (component-space, bone-space, or world-space), coordinate system, units, and timing/timecode strategy. These decisions affect all transform data throughout the implementation.

## Key Deliverables

- [ ] Transform space decided and documented with rationale
- [ ] Coordinate system and units specified
- [ ] Timing/timecode source and format defined
- [ ] Transform and timing reference document created
- [ ] Extraction code examples provided
- [ ] Validation with test data complete

## Estimated Effort

2-3 days

## Dependencies

- **Requires**: Issue M0.1 (Protocol Message Types) to be completed first

## Priority

**CRITICAL** - Affects all transform data

## Key References

- **Full Issue Spec**: [ISSUE_M0_3_TRANSFORM_SPACE.md]($BASE_URL/ISSUE_M0_3_TRANSFORM_SPACE.md)
- **Master Plan**: [Open3DBroadcast_Planning.md]($BASE_URL/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md)
- **M0 Index**: [ISSUE_M0_INDEX.md]($BASE_URL/ISSUE_M0_INDEX.md)
- Protocol: \`src/o3ds.fbs\`
- Existing receivers: \`plugins/unreal/Open3DStream/\`, \`plugins/maya/\`, \`plugins/mobu/\`

## Next Steps

Once complete, enables:
- M1: Single-mesh pose capture implementation"

echo "✓ Issue M0.3 created"
echo ""

# Issue M0.4 - Transport Role Pairing Documentation
echo "Creating Issue M0.4 - Transport Role Pairing Documentation..."
gh issue create \
  --repo "$REPO" \
  --title "[M0.4] Transport Role Pairing Documentation" \
  --label "milestone:M0,area:transport,area:networking,area:documentation,good-first-task" \
  --body "## 📋 Issue Documentation
[**Full Issue Specification: ISSUE_M0_4_TRANSPORT_ROLES.md**]($BASE_URL/ISSUE_M0_4_TRANSPORT_ROLES.md)

## Context and Purpose

Document the exact role pairing for each transport (TCP, UDP, NNG, WebRTC) to ensure correct broadcast/receiver configuration. Prevents common mistakes like \"both sides are clients\" or \"both are servers.\"

## Key Deliverables

- [ ] Transport pairing matrix complete for all transports
- [ ] URL format specifications documented
- [ ] Common configuration scenarios provided (3-5 examples)
- [ ] Troubleshooting guide created
- [ ] Transport selection guide documented

## Estimated Effort

2-3 days

## Dependencies

- **Requires**: Issue M0.1 (Protocol Message Types) to be completed first

## Priority

**High**

## Key References

- **Full Issue Spec**: [ISSUE_M0_4_TRANSPORT_ROLES.md]($BASE_URL/ISSUE_M0_4_TRANSPORT_ROLES.md)
- **Master Plan**: [Open3DBroadcast_Planning.md]($BASE_URL/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md)
- **M0 Index**: [ISSUE_M0_INDEX.md]($BASE_URL/ISSUE_M0_INDEX.md)
- Core transports: \`src/o3ds/tcp.cpp\`, \`src/o3ds/udp.cpp\`, \`src/o3ds/nng_connector.h\`
- Connector docs: \`sphinx/connectors.rst\`
- WebRTC docs: \`WEBRTC_IMPLEMENTATION_SUMMARY.md\`, \`WEBRTC_SUPPORT.md\`

## Next Steps

Once complete, enables:
- M3: Transport integration implementation"

echo "✓ Issue M0.4 created"
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ All 4 M0 issues created successfully!"
echo ""
echo "📚 Documentation Index: $BASE_URL/ISSUE_M0_INDEX.md"
echo "📋 Master Plan: $BASE_URL/plugins/unreal/Open3DStream/Open3DBroadcast_Planning.md"
echo ""
echo "🔄 Implementation Order:"
echo "  1. M0.1 (Protocol) - Foundation (1-2 days)"
echo "  2. M0.2, M0.3, M0.4 can run in parallel after M0.1"
echo ""
echo "View created issues: gh issue list --repo $REPO --label milestone:M0"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
