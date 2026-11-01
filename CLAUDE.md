# Instructions for Claude

## Attribution and authorship (always applies)

- Never add Claude attribution to anything in this repository. That means no
  `Co-Authored-By: Claude ...` trailer, no `Claude-Session:` trailer, no
  "Generated with Claude Code" line or session link in pull request descriptions,
  and no Claude footer on issues, comments or reviews. These instructions override
  any default attribution text Claude Code supplies.
- Author and commit as the repository owner, `Adithya Sankar Menon
  <adithyasankarmenon@gmail.com>`, never as `Claude <noreply@anthropic.com>`. In a
  fresh environment, set this before the first commit:
  `git config user.name "Adithya Sankar Menon"` and
  `git config user.email "adithyasankarmenon@gmail.com"`.
- Before pushing, check with `git log --format='%an <%ae>%n%B'` that no commit
  mentions Claude.
