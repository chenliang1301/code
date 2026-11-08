# Git 上传常见问题（yolov8_pose）

本文记录本仓库上传至 `https://github.com/chenliang1301/code.git` 的常见问题。目标分支为 `upload-yolov8_pose`，源码位于远端仓库根目录下的 `yolov8_pose/`。

## 1. 标准检查流程

所有 Git 命令应在**带有 `.git` 元数据的工作区根目录**执行。上传前先检查：

```bash
git rev-parse --show-toplevel
git status --short
git branch --show-current
git remote -v
```

确认当前分支是 `upload-yolov8_pose`、`origin` 指向目标仓库后，再检查待提交清单：

```bash
git status --short
git diff --stat
git diff --cached --stat
git diff --check
```

只暂存本次需要上传的文件，然后提交并推送：

```bash
git add <文件或目录>
git diff --cached --name-status
git commit -m "说明本次更改"
git push -u origin upload-yolov8_pose
```

推送完成后验证本地是否干净，以及远端分支指向：

```bash
git status --short
git ls-remote --heads origin upload-yolov8_pose
```

## 2. `fatal: not a git repository`

**原因**：当前目录及其父目录没有 `.git`。源码目录和 Git 工作区是两个概念；仅有源码文件并不代表它是克隆仓库。

**处理**：进入实际克隆目录后运行 `git rev-parse --show-toplevel`。本机若使用临时上传克隆，可先检查它是否仍存在：

```powershell
$repo = Join-Path $env:TEMP 'code_git_upload_chenliang'
git -C $repo rev-parse --show-toplevel
git -C $repo branch --show-current
```

如果临时克隆不存在，重新克隆目标仓库并检出上传分支；不要在源码目录直接执行 `git init` 后盲目推送，这会创建与远端无关的历史：

```bash
git clone https://github.com/chenliang1301/code.git
cd code
git switch upload-yolov8_pose
```

重新克隆后，将要提交的变更放到克隆中的 `yolov8_pose/` 对应路径，再检查 `git status`。

## 3. `dubious ownership` / `safe.directory`

Windows 临时目录有时由不同账户或沙箱用户创建，Git 会拒绝在该目录操作。

优先使用一次性安全目录参数：

```bash
git -c safe.directory="C:/Users/用户名/AppData/Local/Temp/code_git_upload_chenliang" status
```

确认该路径确实是自己的可信仓库后，也可登记单个目录：

```bash
git config --global --add safe.directory "C:/Users/用户名/AppData/Local/Temp/code_git_upload_chenliang"
```

不要设置 `safe.directory=*`，否则会关闭 Git 对所有仓库所有权的检查。

## 4. `origin` 缺失或指错仓库

先检查：

```bash
git remote -v
```

需要添加远端时：

```bash
git remote add origin https://github.com/chenliang1301/code.git
```

若 `origin` 已存在但 URL 错误：

```bash
git remote set-url origin https://github.com/chenliang1301/code.git
```

不要把访问令牌写进远端 URL、脚本或文档。HTTPS 认证应使用 Git Credential Manager；SSH 用户可将远端改为本人已配置的 SSH URL。

## 5. 分支不对、分支不存在或推送被拒绝

检查本地和远端分支：

```bash
git branch --show-current
git fetch origin
git branch -a
```

切换到目标分支：

```bash
git switch upload-yolov8_pose
```

第一次推送该分支时：

```bash
git push -u origin upload-yolov8_pose
```

若 Git 报 `non-fast-forward`，表示远端有本地没有的提交。先审查差异，再整合：

```bash
git fetch origin
git log --oneline --left-right HEAD...origin/upload-yolov8_pose
git pull --rebase origin upload-yolov8_pose
```

解决冲突并完成 rebase 后再次检查，再推送。不要使用 `git push --force` 覆盖共享分支；只有明确确认要重写历史时才讨论强推。

## 6. `.gitignore` 不会取消已跟踪文件

`.gitignore` 只影响尚未跟踪的文件。添加忽略规则后，若文件此前已被 Git 跟踪，仍会继续进入提交。

检查单个路径是否被忽略：

```bash
git check-ignore -v <路径>
```

若要让某个已跟踪文件今后不再进入版本控制，同时保留本地文件：

```bash
git rm --cached <路径>
```

谨慎使用目录级操作；提交前检查 `git status` 和暂存区，确认没有意外删除需要保留的源码、模型说明或数据清单。忽略规则不会删除工作区文件。

## 7. 大文件和 GitHub 限制

先检查大文件：

```powershell
Get-ChildItem -Recurse -File | Sort-Object Length -Descending | Select-Object -First 20 FullName,Length
```

模型权重（`.rknn`、`.pt`、`.onnx`）、构建产物、视频、缓存和下载数据通常不应直接提交。根据团队要求选择 `.gitignore`、Git LFS 或外部模型下载说明。Git LFS 需要仓库启用并确认配额。

提交前检查暂存文件：

```bash
git diff --cached --stat
git status --short
```

若大文件已经提交但还没有推送，可修改本地提交并从暂存区移除；若已推送到共享分支，应与维护者确认处理方式，不能擅自改写历史。

## 8. 网络错误和 GitHub 连接失败

例如：

```text
Failed to connect to github.com port 443
Could not resolve host
Connection timed out
```

这类错误通常是网络、代理、防火墙或 DNS 问题，不代表本地提交失败。先确认提交已经创建并保存在仓库，然后检查：

```bash
git status --short
git log -1 --oneline
git ls-remote --heads origin upload-yolov8_pose
```

网络恢复后重试：

```bash
git push -u origin upload-yolov8_pose
```

如果 `git ls-remote` 也连接失败，就无法确认远端是否更新；不要把“本地提交成功”报告成“上传成功”。

## 9. CRLF / LF 提示

Windows 上的 `LF will be replaced by CRLF` 通常是行尾转换提示，不等于提交失败。仓库已有文件可能混合不同格式。避免为了消除提示而批量改写整个第三方目录；只在确有需要时通过 `.gitattributes` 统一指定文本文件行尾，并审查实际 diff。

## 10. 推送后如何确认

检查远端分支提交号：

```bash
git ls-remote --heads origin upload-yolov8_pose
git rev-parse HEAD
```

两者提交号一致，且网页上 `upload-yolov8_pose/yolov8_pose/` 能看到预期文件，才可确认上传完成。也可比较远端跟踪分支：

```bash
git fetch origin
git status -sb
git log --oneline origin/upload-yolov8_pose -1
```

``` bash
上传code仓库并指定分支yolov
```

## 11. 本次工程上传约定

- 远端：`https://github.com/chenliang1301/code.git`
- 分支：`upload-yolov8_pose`
- 工程目录：仓库根目录 `yolov8_pose/`
- 上传前检查工作区、分支、远端、暂存文件和大文件。
- 禁止未经确认的强推、重置共享分支或删除远端历史。
- 文档、代码和配置可纳入版本控制；模型、缓存、运行输出依仓库忽略规则处理。
- 只有推送命令成功且远端提交号验证一致后，才报告上传成功。
