<div id="top"></div>

[![Contributors][contributors-shield]][contributors-url]
[![Forks][forks-shield]][forks-url]
[![Stargazers][stars-shield]][stars-url]
[![Issues][issues-shield]][issues-url]
[![MIT License][license-shield]][license-url]

<br />
<div align="center">
  <a href="https://github.com/N3kOk0/ClipWhale">
    <img src="images/logo.png" alt="Logo" width="80" height="80">
  </a>

<h3 align="center">ClipWhale</h3>

  <p align="center">
    一个简单的剪贴板软件，保存最近两条复制的内容，并通过快捷键粘贴
    <br />
    <br />
    <a href="https://github.com/N3kOk0/ClipWhale/issues">反馈 Bug</a>
    ·
    <a href="https://github.com/N3kOk0/ClipWhale/issues">请求新功能</a>
  </p>
</div>


<details>
  <summary>目录</summary>
  <ol>
    <li><a href="#关于本项目">关于本项目</a></li>
    <li><a href="#安装和使用">安装和使用</a></li>
    <li><a href="#编译">编译</a></li>
    <li><a href="#贡献">贡献</a></li>
    <li><a href="#许可证">许可证</a></li>
    <li><a href="#致谢">致谢</a></li>
  </ol>
</details>


## 关于本项目

[![握握手握握双手][product-screenshot]]()

因为市面上的剪贴板软件都不怎么符合我的需求，所以我用大肥鱼 Vibe Coding 了一个:
- 保存最近两条复制的内容，通过快捷键粘贴
- 常驻后台、低占用
- Win32 原生开发
- 支持深色模式

<p align="right">(<a href="#top">返回顶部</a>)</p>


## 安装和使用

> [!NOTE]
> 需要 Windows 10 以上版本, x64 架构
- 从 [Releases](https://github.com/N3kOk0/ClipWhale/releases) 下载主程序文件
- 双击打开，程序常驻托盘
- 右键托盘图标弹出选项

<p align="right">(<a href="#top">返回顶部</a>)</p>


## 编译

需要 MinGW-w64（`g++` 和 `windres`，[w64devkit](https://github.com/skeeto/w64devkit) 解压即用）和 PowerShell。没有第三方依赖。

```powershell
New-Item -ItemType Directory -Force build, dist

# 资源：图标和 manifest 一起编成 .res.o
windres -I src -O coff -o build/app.res.o src/app.rc

# 编译 + 链接
g++ -std=c++17 -municode -mwindows `
    -finput-charset=UTF-8 -fwide-exec-charset=UTF-16LE -fexec-charset=UTF-8 `
    -Wall -Wextra -Wno-unused-parameter -I src `
    -Os -DNDEBUG -ffunction-sections -fdata-sections `
    -fno-exceptions -fno-rtti `
    -fno-asynchronous-unwind-tables -fno-unwind-tables `
    -fmerge-all-constants -fno-ident `
    src/main.cpp src/util.cpp src/store.cpp src/clipboard.cpp src/settings.cpp src/darkmode.cpp `
    build/app.res.o `
    -o dist/ClipWhale.exe `
    -static -static-libgcc -static-libstdc++ `
    '-Wl,--gc-sections' '-Wl,--nxcompat' '-Wl,--dynamicbase' '-Wl,--high-entropy-va' '-Wl,-s' `
    -lcomctl32 -luxtheme -ldwmapi -lshell32 -luser32 -lgdi32 -ladvapi32
```
- 产物 `dist\ClipWhale.exe`，静态链接，约 232 KB。

<p align="right">(<a href="#top">返回顶部</a>)</p>


## 贡献

贡献让开源社区成为了一个非常适合学习、互相激励和创新的地方。你所做出的任何贡献都是**受人尊敬**的。

如果你有好的建议，请复刻（fork）本仓库并且创建一个拉取请求（pull request）。你也可以简单地创建一个议题（issue），并且添加标签「enhancement」。不要忘记给项目点一个 star！再次感谢！

1. 复刻（Fork）本项目
2. 创建你的 Feature 分支 (`git checkout -b feature/AmazingFeature`)
3. 提交你的变更 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到该分支 (`git push origin feature/AmazingFeature`)
5. 创建一个拉取请求（Pull Request）

<p align="right">(<a href="#top">返回顶部</a>)</p>


## 许可证

根据 MIT 许可证分发。打开 [LICENSE](LICENSE) 查看更多内容。

<p align="right">(<a href="#top">返回顶部</a>)</p>


## 致谢

* [Best-README-Template-zh](https://github.com/songjiahao-wq/Best-README-Template-zh)
* [Deepseek](https://platform.deepseek.com)

<p align="right">(<a href="#top">返回顶部</a>)</p>



<!-- MARKDOWN 链接 & 图片 -->
<!-- https://www.markdownguide.org/basic-syntax/#reference-style-links -->
[contributors-shield]: https://img.shields.io/github/contributors/N3kOk0/ClipWhale.svg?style=for-the-badge
[contributors-url]: https://github.com/N3kOk0/ClipWhale/graphs/contributors
[forks-shield]: https://img.shields.io/github/forks/N3kOk0/ClipWhale.svg?style=for-the-badge
[forks-url]: https://github.com/N3kOk0/ClipWhale/network/members
[stars-shield]: https://img.shields.io/github/stars/N3kOk0/ClipWhale.svg?style=for-the-badge
[stars-url]: https://github.com/N3kOk0/ClipWhale/stargazers
[issues-shield]: https://img.shields.io/github/issues/N3kOk0/ClipWhale.svg?style=for-the-badge
[issues-url]: https://github.com/N3kOk0/ClipWhale/issues
[license-shield]: https://img.shields.io/github/license/N3kOk0/ClipWhale.svg?style=for-the-badge
[license-url]: https://github.com/N3kOk0/ClipWhale/blob/main/LICENSE
[product-screenshot]: images/screenshot.png
