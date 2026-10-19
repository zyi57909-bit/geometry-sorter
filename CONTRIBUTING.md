# 协作规范（三人小组 + GitHub）

> 给第一次用 git/GitHub 的组员：**只看 §1~§4 就够用**，命令可以直接复制。
> 主控（仓库 owner）额外看 §5~§8。

## 0 一句话规则

**main 分支永远能编译；每个人的改动走自己的分支 + Pull Request（PR）；接口改动必须先改 `00_接口约定/`。**

## 1 分工（谁改哪里，这条决定了几乎不会有冲突）

| 目录 | 谁负责 | 说明 |
|---|---|---|
| `00_接口约定/` | 主控 | 三人确认后由主控改；别人提 Issue 或 PR 讨论 |
| `01_计划/`、`02_决策记录/` | 主控 | 计划与决策只在主控这边更新 |
| `10_主控_Arduino/` | 主控 | 固件；**只有这里允许编译/烧录** |
| `20_视觉_K230/` | 视觉同学 | 只改这个目录（`det_video.py`、`待收/`） |
| `30_HMI_串口屏/` | HMI 同学 | 只改这个目录（`.HMI`、`.tft`、`待收/`） |
| `90_历史存档/`、`91_文档快照/` | **谁都不改** | 只读证据；要旧版本用 `git show v0.1-baseline:<路径>` |
| `99_调试记录/` | 主控 | 每人也可以建自己的子目录 `99_调试记录/<日期>_<名字>/` |

**三个人改的是不同目录 → 几乎不会冲突。唯一的共享文件是 `00_接口约定/`，它走 PR 流程。**

## 2 第一次使用（只做一次）

> **仓库已 public（2026-10-18）：不需要邀请。** 任何人拿到链接就能看代码、clone、提 Issue 和 PR。
> 只有"直接往本仓库推分支"才需要权限——两种做法见本节末尾。

**1. 注册 GitHub 账号**（已有可跳过），把用户名发给主控，方便认人和分配 Issue。

**2. 装工具**（任选）：**GitHub Desktop**（新手推荐，图形界面 + 自带登录）或命令行 Git。

**3. 配置 git 身份**（命令行 Git 必须做，只做一次；否则提交作者是空的或乱码）：

```bash
git config --global user.name  "你的 GitHub 用户名"
git config --global user.email "你的 GitHub 邮箱"   # 建议用 GitHub 的 noreply 邮箱
```

**4. 配置 GitHub 认证**（**最容易卡住的一步**：GitHub 早已不允许用登录密码 push）：

| 方式 | 怎么做 |
|---|---|
| GitHub Desktop | 打开后 `Sign in to GitHub.com` 登录一次，之后自动记住 |
| 命令行 + Git Credential Manager | 第一次 `git push` 会自动弹浏览器让授权，登录后凭据被系统记住 |
| 手动 PAT | GitHub → Settings → Developer settings → Personal access tokens → 勾 `repo` → push 时把 token 当密码填 |

**5. 克隆到自己电脑**：

```bash
git clone https://github.com/zyi57909-bit/geometry-sorter.git
cd geometry-sorter
git config core.hooksPath .githooks     # 装防误推 main 的 pre-push 钩子
```

> Windows 上等价做法：`.\tools\install-hooks.ps1`。
> 改文件之前先自检三条：`git config user.name` / `git config user.email` 都有值、`git status` 正常、`git push` 能推上去。

**要"直接推分支"的两种做法：**

- **加协作者**：主控在仓库 `Settings → Collaborators → Add people` 输入你的 GitHub 用户名，之后按 §3 直接 `git push -u origin HEAD`
- **Fork 流程**（不想被加也能用）：仓库页右上角 **Fork** → clone 你自己的 fork → 改动推到自己 fork → 从 fork 向本仓库开 PR。**公开仓库用 fork 提 PR 完全不需要邀请**

> ⚠️ 公开仓库 = 全世界可见：**不要提交密码、token、个人隐私路径，也不要替别人提交未公开的工程文件**；提交历史里的邮箱同样公开，介意就用 noreply 邮箱。
> ⚠️ 不要把仓库放在 **OneDrive/网盘同步目录** 里，也不要让编辑器一直占着二进制文件——之前 `ji.HMI` 被进程锁死，导致读/改/入库全部失败。

## 2.5 分支保护（已启用）

2026-10-18：仓库已改为 **public**（免费账号的 public 仓库支持分支保护），`main` 保护已开启：

| 保护项 | 状态 |
|---|---|
| 必须走 PR 才能合并 | ✅ 是 |
| 禁止 force push | ✅ 是 |
| 禁止删除分支 | ✅ 是 |
| 必须解决所有对话 | ✅ 是 |
| 需要 N 人 approve | 当前 **0**（可以自己合并自己的 PR） |
| 管理员也受限制 | ❌ 否（`enforce_admins=false`，紧急时可绕过） |

- 想更严：`.\tools\enable-branch-protection.ps1 -RequireApprovals 1`，或网页 Settings → Branches 里改
- 本地还有 **pre-push 钩子**做双保险：每台电脑 clone 后跑一次 `.\tools\install-hooks.ps1`
- 紧急绕过（要用就说明原因）：`ALLOW_MAIN_PUSH=1 git push`

### 仓库转公开后的注意事项

- `ji.HMI`（同学作品）、`det_video.py`、AI 运行日志等**都对外可见**，建议告知两位同学
- 如果有人不同意：Settings → General → Danger Zone 可改回 private，但**已被 fork / 被搜索引擎缓存的内容收不回来**
- 仓库历史里含 Windows 用户名 `aochaoji` 和部分提交邮箱；要彻底清理需要重写 git 历史（找主控）
- 没有 `LICENSE` = 默认"保留所有权利"，别人不能直接复用；打算开源就补一个许可证

## 3 日常改动五步（命令行）

## 3 日常改动五步（命令行）

```bash
git pull                                          # 1) 先拿别人最新的
git switch -c feat/vision-digital-id              # 2) 新建自己的分支（名字自取）
# ...改文件...
git status                                        # 3) 看改了哪些
git add -A && git commit -m "feat(vision): 回包改数字 id"   # 4) 提交
git push -u origin HEAD                           # 5) 推上去
```

推上去后打开仓库网页，会出现 **Compare & pull request** → 开 PR → 等 review → 合并。

**GitHub Desktop 等价操作**：`Current branch → New branch` → 改文件 → `Commit to ...` → `Push origin` → `Create Pull Request`。

合并后回到 main：

```bash
git switch main && git pull
```

## 4 PR 规矩（很简单，但请照做）

- 提 PR 时页面会**自动带出 PR 模板**（`.github/pull_request_template.md`），照着填空；**"证据"一节不填的 PR 不合并**
- 标题写清改什么；描述里贴**证据**：日志片段 / 截图 / `compile.ps1` 结果
- 至少 1 人 review 后再合并（主控合并 `10_`、`00_`；视觉/HMI 的改动由主控或对方 review）
- **main 只接受 PR**，不要直接往 main push
- 合并方式选 **Squash and merge**（历史干净，一次 PR 一个提交）
- 合并前不要 force push

## 4.5 什么时候开 Issue（3 个模板）

| 场景 | 用哪个模板 | 在哪 |
|---|---|---|
| 要改接口（回包 / 波特率 / 时序 / 参数 / 控件） | **接口变更** | 仓库 **Issues → New issue** |
| 实机 / 联调出故障 | **故障报告**（必须贴日志 + "已排除项"） | 同上 |
| 阶段计划里的具体任务 | **任务 / 待办** | 同上 |

规矩：**"要对方确认的事"一律变成 Issue，不要只在群里口头说**——Issue 有记录、能 @ 人、能关闭、能关联 PR。

## 5 冲突怎么办（新手最怕的部分）

- 冲突只会发生在"两个人改了同一个文件的同一处"。按 §1 分工，正常情况**几乎不会发生**
- 发生时的命令行处理：

```bash
git pull          # 报 conflict
# 打开冲突文件，搜索 <<<<<<<，手工留下正确内容，删掉标记
git add 冲突文件
git commit -m "fix: resolve conflict"
```

- 实在不会：GitHub Desktop 会提示 `Open in editor`；或者把冲突截图发群里，**别乱点"覆盖对方"**
- 消灭冲突的规矩：**`00_接口约定/` 同一时间只有一个人改**，谁改谁先在群里说一声

## 6 绝对不要提交的东西

- `_build/`、`*.hex`、`*.elf`、`*.bin`（已在 `.gitignore` / `.gitattributes` 处理）
- 单个 **>10 MB** 的文件（`ji.HMI` 9.5 MB 已经进库了，别再往里加大文件；`.tft`、`.kmodel` 走 GitHub Release 或 Git LFS）
- 密码 / token / 个人电脑路径
- 别人还没确认的工程文件（视觉/HMI 自己提 PR）

## 7 提交信息格式

`类型(范围): 说明`，类型用 `feat / fix / docs / chore / build / test`：

```text
feat(ams): 加 NONE 分支与 r==0 重试计数
docs(interface): 接口约定升 v0.2 并冻结
fix(vision): color_four 取模防越界
```

一次提交只做一件事；别把"改代码 + 改三份文档"混在一个提交里。

## 8 版本、交付与回滚

| 场景 | 做法 |
|---|---|
| 接口冻结 | 打 tag：`git tag -a v0.2-接口冻结 -m "..."` + `git push --tags` |
| 正式交付 | tag `v1.0-交付` + 在 GitHub 建 **Release**，附 `.hex`、`.tft`、交付清单 |
| 撤销某次改动 | `git revert <commit>`（**不要** reset 已经推送的历史） |
| 找回旧版本 | `git show v0.1-baseline:<路径>` |

## 9 出问题找谁

| 问题 | 找 |
|---|---|
| git/GitHub 操作、分支冲突 | 主控 |
| 硬件、接线、烧录、固件 | 主控 |
| 视觉识别、K230、类别表 | 视觉同学 |
| 串口屏、控件、波特率 | HMI 同学 |
