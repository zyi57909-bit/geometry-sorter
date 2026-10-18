#requires -Version 5.1
<#
.SYNOPSIS
    Enable server-side branch protection on 'main' (require a pull request).

.DESCRIPTION
    Calls the GitHub REST API (PUT /repos/{owner}/{repo}/branches/main/protection).

    IMPORTANT: GitHub Free does NOT allow branch protection on PRIVATE repositories.
    Running this on a private repo of a Free account returns HTTP 403
    ("Upgrade to GitHub Pro or make this repository public"). It works after:
      - upgrading to GitHub Pro (free for students via GitHub Student Developer Pack), or
      - making the repository public.

    Usage:
      .\tools\enable-branch-protection.ps1
      .\tools\enable-branch-protection.ps1 -Repo "owner/name" -RequireApprovals 1

    NOTE: this file is ASCII-only on purpose (Windows PowerShell 5.1 reads
    BOM-less .ps1 as ANSI; non-ASCII would break parsing).
#>
param(
    [string]$Repo = 'zyi57909-bit/geometry-sorter',
    [int]$RequireApprovals = 0
)

$ErrorActionPreference = 'Stop'

$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) {
    $cand = 'C:\Program Files\GitHub CLI\gh.exe'
    if (Test-Path -LiteralPath $cand) { $gh = $cand } else { throw 'gh CLI not found' }
}

$body = @{
    required_status_checks = $null
    enforce_admins = $false
    required_pull_request_reviews = @{
        dismiss_stale_reviews = $true
        require_code_owner_reviews = $false
        required_approving_review_count = $RequireApprovals
    }
    restrictions = $null
    allow_force_pushes = $false
    allow_deletions = $false
    required_conversation_resolution = $true
} | ConvertTo-Json -Depth 5

$tmp = Join-Path $env:TEMP 'branch-protection.json'
# NOTE: use ASCII (no BOM). 'Set-Content -Encoding utf8' on PowerShell 5.1 writes a BOM,
# and gh/Go rejects BOM-prefixed JSON with "Problems parsing JSON".
Set-Content -LiteralPath $tmp -Value $body -Encoding ascii

Write-Host "== PUT repos/$Repo/branches/main/protection (approvals=$RequireApprovals)" -ForegroundColor Cyan
& $gh api --method PUT "repos/$Repo/branches/main/protection" --input $tmp
if ($LASTEXITCODE -ne 0) {
    throw "failed exit=$LASTEXITCODE. If this is a private repo on GitHub Free, upgrade to Pro (student pack) or make it public, then retry."
}

Write-Host 'Branch protection enabled on main.' -ForegroundColor Green
