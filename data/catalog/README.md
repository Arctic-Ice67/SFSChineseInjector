# 词表格式（data/catalog/*.json）

一个 DLL mod 一份词表，文件名建议用程序集名小写，例如 `viewcontrol.json` 对应 `ViewControl.dll`。

```json
{
  "schema": 1,
  "assembly": "ViewControl",
  "display": "View Control",
  "entries": {
    "<原文，必须与 DLL 里的字面量完全一致>": {
      "zh": "<中文译文>",
      "state": "approved | draft | skip",
      "note": "用途说明（给自己看）"
    }
  },
  "skip": ["<明确不翻的原文>", "..."]
}
```

## 规则

1. **只翻 entries 里列出的东西**，工具不会猜着翻；没列出的字符串一律原样保留。
2. `state`：
   - `approved`（默认）：会被写入
   - `draft`：暂存，不写入（UI 里可见，方便以后补）
   - `skip`：明确不翻（等效于放进 skip 数组）
3. **长度上限**：译文（UTF-16）字节数必须 ≤ 原文字节数。
   中文通常只有英文的一半甚至更少，一般不会超；超了工具会明确报错并拒绝。
   不足的部分自动用 U+200B（零宽空格）补齐，**不改变文件结构与长度**。
4. `skip` 里放：GameObject/Transform 名字、反射字段名、日志前缀、程序集名、
   版本号、路径、配置键等 —— 翻了会导致 mod 找不到对象或反射失败。
5. 拼接型的提示（如 `"View Control: started " + n + " rocket(s)"`）要把碎片一起翻，
   各自保持末尾/开头的空格，这样拼起来才是通顺的中文。

## 哪些字符串不能翻（踩过的坑）

| 类型 | 例子 | 后果 |
|---|---|---|
| Unity 对象名 | `Stages Holder`、`ViewControl Title` | `transform.Find` 返回 null，面板不显示 |
| 反射字段/方法名 | `selected`、`part` | `GetField/GetMethod` 抛异常 |
| 日志前缀 | `[ViewControl] ` | 只影响日志，但没必要动 |
| 程序集/包名 | `com.arcticice.viewcontrol` | ModLoader 识别不到 mod |
| 版本号 | `v0.1.0`、`1.5.10.2` | 版本判断出错 |
| 配置键 / 存档变量名 | `basics`、`NUMBER_VARIABLES` | 读不到配置、存档异常 |

工具会自动给「同一文本也出现在元数据标识符堆（#Strings）里」的条目打上 `ident` 标记，
这类基本都是标识符，默认不建议翻。
