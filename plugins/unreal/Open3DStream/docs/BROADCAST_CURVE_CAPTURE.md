# Broadcast Curve Capture (M0.2)

Sources
- Morph targets from `USkeletalMesh` (names from mesh morph list)
- Named animation curves declared on `USkeleton` metadata

Name set and ordering
- Initial set built at BeginPlay or mesh/skeleton change
- Optional include/exclude wildcard patterns applied (case-sensitive, `*` and `?` supported)
- Deterministic order: lexicographic stable sort by name
- Default behavior (recommended for M2): stable name set; send full curves each frame

Value capture
- Component state after post-evaluation (`RegisterOnBoneTransformsFinalizedDelegate`)
- Source priority per name index:
  1) Component morph override map (`GetMorphTargetCurves()`)
  2) Component final curve value (`GetCurveValue(Name, 0.0f, OutVal)`)
  3) Fallback for morphs: `GetMorphTarget(Name)`
- Morph clamping: optional `bClampMorphCurvesToUnit` clamps to [0..1]
- NaN/Inf guard: drop individual curves during filtering when enabled

Filtering behavior (proposed/documented)
- Default: `bEnableCurveFiltering=false` ? always send full curve name set and values; indices stable; receiver static data unchanged
- When `bEnableCurveFiltering=true`:
  - Include/exclude patterns applied
  - Epsilon filtering: drop |v| < `CurveEpsilon`
  - Delta filtering: drop |v - lastSent| < `CurveDeltaThreshold`
  - Trade-off: Dropping entries changes the per-frame curve name set. Receivers like LiveLink will detect property-name changes and may refresh static data, causing overhead
  - Recommendation for M2: leave filtering disabled for stable subjects; enable only for bandwidth-sensitive testing
  - Future option: keep name set stable and send zeros for dropped values (M3+)

Acceptance notes
- With filtering off, property order/name set is stable across frames; hash computed on receiver remains constant
- With filtering on, expect static data churn; document clearly for users

CVars and settings
- See `UO3DSBroadcastComponent` properties:
  - `bClampMorphCurvesToUnit`, `bDropNaNAndInfinity`, `bEnableCurveFiltering`
  - `CurveEpsilon`, `CurveDeltaThreshold`, `IncludeCurvePatterns`, `ExcludeCurvePatterns`, `bLogFilteredCurves`

References
- `Plugins/Open3DStream/Source/Open3DBroadcast/Private/O3DSBroadcastComponent.cpp`
- `Plugins/Open3DStream/Source/Open3DBroadcast/Public/O3DSBroadcastComponent.h`
