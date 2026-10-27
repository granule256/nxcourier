# 保留的设备成品

这里的 `exefs.nsp` 沿用本次更新前的远端main成品，本次没有替换它：

- 长度：229103字节。
- SHA256：`fc5645d6ba53d8f4240d40a1f6e7aff034bb57b1b01af500dcaff2487f4e4a8d`。
- 旧设备能力：14工具54方法；实际运行版本以 `identify.id` 和 `tools.list` 为准。

当前设备源码包含新fs、队列与存档能力，与此成品不同。请按仓库构建说明生成 `device/platform/nxc.nsp`，完成目标固件真机验收后再更新发行成品；不要将本地候选直接当作已验收版本。

`boot2.flag`、`toolbox.json` 与 `config/nxc/` 为安装配套文件。安装时启动标记放入模块目录的 `flags/boot2.flag`；配置复制到SD卡的 `/config/nxc/`。后者控制监听和睡眠收口，不能省略。
