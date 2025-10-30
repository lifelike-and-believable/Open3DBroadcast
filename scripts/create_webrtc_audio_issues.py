#!/usr/bin/env python3
"""
Create WebRTC Audio Refactor GitHub Issues

This script parses WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md and creates
the Epic and 7 sub-issues in GitHub using the GitHub CLI (gh).

Usage:
    python3 scripts/create_webrtc_audio_issues.py [--repo OWNER/REPO] [--dry-run]

Requirements:
    - GitHub CLI (gh) installed and authenticated
    - Permission to create issues in the target repository

Author: Open3DStream Team
Date: 2025-10-30
"""

import argparse
import os
import re
import subprocess
import sys
from typing import Dict, List, Optional, Tuple


class IssueSpec:
    """Represents a parsed issue specification."""
    
    def __init__(self, number: int, title: str, labels: List[str], body: str):
        self.number = number
        self.title = title
        self.labels = labels
        self.body = body
        self.is_epic = (number == 8)
    
    def __repr__(self):
        return f"IssueSpec({self.number}, {self.title[:50]}...)"


def check_gh_installed() -> bool:
    """Check if GitHub CLI is installed and authenticated."""
    try:
        result = subprocess.run(['gh', '--version'], 
                              capture_output=True, 
                              text=True, 
                              check=True)
        print(f"✓ GitHub CLI found: {result.stdout.split()[2]}")
        
        # Check authentication
        result = subprocess.run(['gh', 'auth', 'status'],
                              capture_output=True,
                              text=True,
                              check=False)
        
        if result.returncode != 0:
            print("✗ GitHub CLI is not authenticated")
            print("  Run: gh auth login")
            return False
        
        print("✓ GitHub CLI is authenticated")
        return True
        
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("✗ GitHub CLI (gh) is not installed")
        print("  Install from: https://cli.github.com/")
        return False


def parse_issue_file(filepath: str) -> List[IssueSpec]:
    """Parse the detailed issue specification file."""
    
    if not os.path.exists(filepath):
        print(f"✗ File not found: {filepath}")
        sys.exit(1)
    
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Split into issue sections
    issue_pattern = re.compile(r'^## Issue (\d+):\s*(.+?)$', re.MULTILINE)
    matches = list(issue_pattern.finditer(content))
    
    if len(matches) != 8:
        print(f"✗ Expected 8 issues, found {len(matches)}")
        sys.exit(1)
    
    issues = []
    
    for i, match in enumerate(matches):
        issue_num = int(match.group(1))
        issue_title_raw = match.group(2).strip()
        
        # Extract the section for this issue
        start_pos = match.end()
        end_pos = matches[i + 1].start() if i + 1 < len(matches) else len(content)
        section = content[start_pos:end_pos].strip()
        
        # Find the title field (may be different from header)
        title_match = re.search(r'^### Title\s*\n`(.+?)`', section, re.MULTILINE)
        if title_match:
            issue_title = title_match.group(1)
        else:
            issue_title = issue_title_raw
        
        # Extract labels (they're in individual backticks like `label1`, `label2`)
        labels_match = re.search(r'^### Labels\s*\n(.+?)$', section, re.MULTILINE)
        if labels_match:
            labels_line = labels_match.group(1)
            # Extract all text within backticks
            labels = re.findall(r'`([^`]+)`', labels_line)
        else:
            labels = ['area:unreal', 'area:webrtc', 'audio']
        
        # Extract body (everything from after title/labels to the --- separator)
        # Find where the actual content starts (after title and labels)
        body_start = section
        
        # Remove the title line
        body_start = re.sub(r'^### Title\s*\n`[^`]+`\s*\n', '', body_start, count=1)
        # Remove the labels line
        body_start = re.sub(r'^### Labels\s*\n`[^`]+`\s*\n', '', body_start, count=1)
        
        # Find the ending --- separator and remove it
        body = re.sub(r'\n---\s*$', '', body_start).strip()
        
        issues.append(IssueSpec(issue_num, issue_title, labels, body))
    
    return issues


def create_issue(repo: str, title: str, labels: List[str], body: str, dry_run: bool = False) -> Optional[int]:
    """Create a GitHub issue using gh CLI."""
    
    labels_str = ','.join(labels)
    
    if dry_run:
        print(f"  [DRY RUN] Would create issue:")
        print(f"    Title: {title}")
        print(f"    Labels: {labels_str}")
        print(f"    Body length: {len(body)} chars")
        return 999  # Fake issue number for dry run
    
    # Create issue using gh CLI
    cmd = [
        'gh', 'issue', 'create',
        '--repo', repo,
        '--title', title,
        '--label', labels_str,
        '--body', body
    ]
    
    try:
        result = subprocess.run(cmd, 
                              capture_output=True, 
                              text=True, 
                              check=True)
        
        # Extract issue number from output (format: https://github.com/owner/repo/issues/123)
        url = result.stdout.strip()
        issue_num_match = re.search(r'/issues/(\d+)', url)
        
        if issue_num_match:
            return int(issue_num_match.group(1))
        else:
            print(f"  Warning: Could not extract issue number from: {url}")
            return None
            
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Failed to create issue: {e.stderr}")
        return None


def update_epic_with_issues(repo: str, epic_num: int, sub_issue_nums: List[Tuple[int, int]], 
                            epic_body: str, dry_run: bool = False) -> bool:
    """Update the Epic issue body to include links to sub-issues."""
    
    # Build the updated sub-issues section
    phase_map = {
        1: "Phase A: Unmask and Contain",
        2: "Phase A: Unmask and Contain",
        3: "Phase B: Centralize and Order",
        4: "Phase B: Centralize and Order",
        5: "Phase C: Diagnostics and Tests",
        6: "Phase C: Diagnostics and Tests",
        7: "Phase D: Cleanup",
    }
    
    # Group issues by phase
    phases = {}
    for orig_num, gh_num in sub_issue_nums:
        phase = phase_map.get(orig_num, "Unknown")
        if phase not in phases:
            phases[phase] = []
        phases[phase].append((orig_num, gh_num))
    
    # Build checklist
    checklist_lines = ["#### Sub-Issues", ""]
    for phase in ["Phase A: Unmask and Contain", 
                  "Phase B: Centralize and Order",
                  "Phase C: Diagnostics and Tests",
                  "Phase D: Cleanup"]:
        if phase in phases:
            checklist_lines.append(f"### {phase}")
            for orig_num, gh_num in sorted(phases[phase]):
                checklist_lines.append(f"- [ ] #{gh_num}")
            checklist_lines.append("")
    
    checklist = '\n'.join(checklist_lines)
    
    # Replace the placeholder sub-issues section in epic body
    updated_body = re.sub(
        r'#### Sub-Issues.*?(?=###|$)',
        checklist,
        epic_body,
        flags=re.DOTALL
    )
    
    if dry_run:
        print(f"  [DRY RUN] Would update Epic #{epic_num} with sub-issue links")
        return True
    
    # Use gh CLI to edit the issue
    cmd = ['gh', 'issue', 'edit', str(epic_num), '--repo', repo, '--body', updated_body]
    
    try:
        subprocess.run(cmd, capture_output=True, text=True, check=True)
        return True
    except subprocess.CalledProcessError as e:
        print(f"  ✗ Failed to update Epic: {e.stderr}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Create WebRTC Audio Refactor issues from detailed spec',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Create issues in the default repository
  python3 scripts/create_webrtc_audio_issues.py
  
  # Dry run to see what would be created
  python3 scripts/create_webrtc_audio_issues.py --dry-run
  
  # Create issues in a different repository
  python3 scripts/create_webrtc_audio_issues.py --repo myorg/myrepo
        """
    )
    
    parser.add_argument('--repo', 
                       default='lifelike-and-believable/Open3DStream',
                       help='GitHub repository (default: lifelike-and-believable/Open3DStream)')
    
    parser.add_argument('--spec-file',
                       default='WEBRTC_AUDIO_REFACTOR_ISSUES_DETAILED.md',
                       help='Path to issue specification file')
    
    parser.add_argument('--dry-run',
                       action='store_true',
                       help='Parse and validate without creating issues')
    
    args = parser.parse_args()
    
    print("=" * 70)
    print("WebRTC Audio Refactor - Issue Creation")
    print("=" * 70)
    print()
    
    # Check prerequisites
    if not args.dry_run and not check_gh_installed():
        sys.exit(1)
    
    print()
    print(f"Repository: {args.repo}")
    print(f"Spec file: {args.spec_file}")
    if args.dry_run:
        print("Mode: DRY RUN (no issues will be created)")
    print()
    
    # Parse issue specifications
    print(f"Parsing issue specifications from {args.spec_file}...")
    issues = parse_issue_file(args.spec_file)
    
    epic = [i for i in issues if i.is_epic][0]
    sub_issues = [i for i in issues if not i.is_epic]
    
    print(f"✓ Found {len(issues)} issues (1 Epic + {len(sub_issues)} sub-issues)")
    print()
    
    # Create Epic first
    print("Creating Epic issue...")
    print(f"  Title: {epic.title}")
    print(f"  Labels: {', '.join(epic.labels)}")
    
    epic_num = create_issue(args.repo, epic.title, epic.labels, epic.body, args.dry_run)
    
    if epic_num:
        print(f"✓ Epic created: #{epic_num} - {epic.title}")
    else:
        print("✗ Failed to create Epic issue")
        sys.exit(1)
    
    print()
    
    # Create sub-issues
    print("Creating sub-issues...")
    created_issues = []
    
    for issue in sorted(sub_issues, key=lambda i: i.number):
        print(f"  Creating Issue {issue.number}: {issue.title[:60]}...")
        
        # Prepend Epic reference to body
        body_with_epic = f"Part of Epic: #{epic_num}\n\n{issue.body}"
        
        issue_num = create_issue(args.repo, issue.title, issue.labels, body_with_epic, args.dry_run)
        
        if issue_num:
            print(f"  ✓ Issue {issue.number} created: #{issue_num}")
            created_issues.append((issue.number, issue_num))
        else:
            print(f"  ✗ Failed to create Issue {issue.number}")
    
    print()
    
    # Update Epic with sub-issue links
    if created_issues:
        print("Updating Epic with sub-issue links...")
        if update_epic_with_issues(args.repo, epic_num, created_issues, epic.body, args.dry_run):
            print(f"✓ Epic #{epic_num} updated with sub-issue checklist")
        else:
            print(f"✗ Failed to update Epic #{epic_num} (issues still created successfully)")
    
    print()
    print("=" * 70)
    print("Summary")
    print("=" * 70)
    
    if args.dry_run:
        print("DRY RUN completed successfully")
        print(f"Would create {len(issues)} issues")
    else:
        print("All issues created successfully!")
        print(f"Epic: #{epic_num}")
        print(f"Sub-issues: {', '.join(f'#{num}' for _, num in sorted(created_issues))}")
        print()
        print(f"View Epic: https://github.com/{args.repo}/issues/{epic_num}")
    
    print()


if __name__ == '__main__':
    main()
