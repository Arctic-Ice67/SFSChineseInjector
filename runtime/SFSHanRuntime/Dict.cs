using System;
using System.Collections.Generic;
using System.IO;
using Newtonsoft.Json.Linq;

namespace SFSHanRuntime
{
    /// <summary>
    /// 运行时词表。dict.json 结构：
    /// {
    ///   "exact":  { "原文": "译文", ... },               // 整串精确匹配
    ///   "prefix": [ { "from": "前缀", "to": "译文前缀" } ], // 前缀替换
    ///   "inline": [ { "from": "子串", "to": "替换" } ]     // 子串替换（拼接型文本）
    /// }
    /// 运行时装模式可从内存载入（LoadFromText），文件模式支持热更新。
    /// </summary>
    internal static class Dict
    {
        private static readonly Dictionary<string, string> Exact =
            new Dictionary<string, string>(StringComparer.Ordinal);

        private static readonly List<KeyValuePair<string, string>> Prefix =
            new List<KeyValuePair<string, string>>();

        private static readonly List<KeyValuePair<string, string>> Inline =
            new List<KeyValuePair<string, string>>();

        private static string _path;
        private static DateTime _mtime = DateTime.MinValue;
        private static bool _dirty;

        public static int Count { get { return Exact.Count; } }
        public static int PrefixCount { get { return Prefix.Count; } }
        public static int InlineCount { get { return Inline.Count; } }
        public static string Path_ { get { return _path; } }

        // ---------- 载入 ----------

        /// <summary>文件模式：从 mod 目录读 dict.json，支持 Watcher 热更新。</summary>
        public static void Load(string modFolder)
        {
            _path = System.IO.Path.Combine(modFolder ?? "", "dict.json");
            Reload();
        }

        /// <summary>内存模式：直接吃一段 JSON 文本，不碰文件。</summary>
        public static void LoadFromText(string json)
        {
            _path = null;
            try { Parse(json); }
            catch (Exception e) { Log.Err("LoadFromText: " + e.Message); }
        }

        public static void Reload()
        {
            try
            {
                if (string.IsNullOrEmpty(_path) || !File.Exists(_path)) return;
                _mtime = File.GetLastWriteTimeUtc(_path);
                Parse(File.ReadAllText(_path));
            }
            catch (Exception e)
            {
                Log.Err("Reload: " + e.Message);
            }
        }

        /// <summary>有变化时重载；由 Watcher 每 2 秒调用（内存模式自动跳过）。</summary>
        public static void Poll()
        {
            if (string.IsNullOrEmpty(_path)) return;
            try
            {
                if (!File.Exists(_path)) return;
                var m = File.GetLastWriteTimeUtc(_path);
                if (m != _mtime) Reload();
            }
            catch (Exception e)
            {
                Log.Err("Poll: " + e.Message);
            }
        }

        private static void Parse(string json)
        {
            var root = JObject.Parse(json);
            var exact = new Dictionary<string, string>(StringComparer.Ordinal);
            var prefix = new List<KeyValuePair<string, string>>();
            var inline = new List<KeyValuePair<string, string>>();

            var ex = root["exact"] as JObject;
            if (ex != null)
            {
                foreach (var kv in ex.Properties())
                {
                    var v = kv.Value.Type == JTokenType.String ? (string)kv.Value : null;
                    if (!string.IsNullOrEmpty(kv.Name) && !string.IsNullOrEmpty(v)) exact[kv.Name] = v;
                }
            }
            var pr = root["prefix"] as JArray;
            if (pr != null)
            {
                foreach (var it in pr)
                {
                    var from = (string)it["from"];
                    var to = (string)it["to"];
                    if (!string.IsNullOrEmpty(from) && to != null)
                        prefix.Add(new KeyValuePair<string, string>(from, to));
                }
                prefix.Sort((a, b) => b.Key.Length.CompareTo(a.Key.Length));  // 长前缀优先
            }
            var il = root["inline"] as JArray;
            if (il != null)
            {
                foreach (var it in il)
                {
                    var from = (string)it["from"];
                    var to = (string)it["to"];
                    if (!string.IsNullOrEmpty(from) && to != null)
                        inline.Add(new KeyValuePair<string, string>(from, to));
                }
                inline.Sort((a, b) => b.Key.Length.CompareTo(a.Key.Length));
            }

            Exact.Clear();
            foreach (var kv in exact) Exact[kv.Key] = kv.Value;
            Prefix.Clear();
            Prefix.AddRange(prefix);
            Inline.Clear();
            Inline.AddRange(inline);
            _dirty = true;
            Log.Info("词表已载入：精确 " + Exact.Count + " 条，前缀 " + Prefix.Count +
                     " 条，子串 " + Inline.Count + " 条");
        }

        public static bool WasReloaded()
        {
            if (!_dirty) return false;
            _dirty = false;
            return true;
        }

        // ---------- 翻译 ----------

        /// <summary>核心翻译入口：命中就返回中文，否则原样返回。</summary>
        public static string Translate(string value)
        {
            if (string.IsNullOrEmpty(value)) return value;
            if (Exact.Count == 0 && Prefix.Count == 0 && Inline.Count == 0) return value;

            string hit;
            if (Exact.TryGetValue(value, out hit)) return hit;

            // 有些 mod 会往字符串里塞零宽字符（U+200B）或首尾空格，一模一样比对会漏
            if (value.IndexOf('\u200B') >= 0 || value.IndexOf('\uFEFF') >= 0 ||
                value.Length != value.Trim().Length)
            {
                string cleaned = value.Replace("\u200B", "").Replace("\uFEFF", "").Trim();
                if (cleaned.Length > 0 && Exact.TryGetValue(cleaned, out hit)) return hit;
            }

            for (int i = 0; i < Prefix.Count; i++)
            {
                var p = Prefix[i];
                if (p.Key.Length == 0 || value.Length < p.Key.Length) continue;
                if (string.CompareOrdinal(value, 0, p.Key, 0, p.Key.Length) == 0)
                    return string.Concat(p.Value, value.Substring(p.Key.Length));
            }

            // 子串替换：只对「像拼接文本」的字符串做（含换行或冒号），避免拖慢普通 UI
            if (Inline.Count > 0 && (value.IndexOf('\n') >= 0 || value.IndexOf(':') >= 0))
            {
                string s = value;
                for (int i = 0; i < Inline.Count; i++)
                {
                    var r = Inline[i];
                    if (r.Key.Length == 0) continue;
                    if (s.IndexOf(r.Key, StringComparison.Ordinal) >= 0) s = s.Replace(r.Key, r.Value);
                }
                if (!ReferenceEquals(s, value)) return s;
            }
            return value;
        }
    }
}
