---
id: O3D-002
title: Copyright line on plugin source files
level: MUST
scope: project
paths: ["**/*.h", "**/*.cpp", "**/*.inl", "**/*.cs"]
verified-by: [ci]
check: "ci: Copyright headers (Build/Scripts/check-copyright-headers.py)"
targets-failure: convention-drift
observed-on: []
rationale: Two copyright holders are accepted and vendored code keeps its own notice; replaces FAB-002.
---
Start each new plugin source file under `Source/` with `// Copyright Lifelike & Believable. All Rights Reserved.` followed by a blank line. Leave files that start with `// Copyright (c) Open3DStream Contributors` as they are, and leave third-party files under any `ThirdParty/` folder and generated files untouched. Run `python3 Build/Scripts/check-copyright-headers.py` and include its output.
