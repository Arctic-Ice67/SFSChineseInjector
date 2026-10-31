using System;
using System.Collections.Generic;
using System.IO;
using SFS.Translations;
using TMPro;
using UnityEngine.TextCore.LowLevel;
using UnityEngine;

namespace SFSHanRuntime
{
    /// <summary>
    /// 字体安装：把我们的中文字体做成 TMP 字体资源并套到所有文本上。
    /// 默认走「替换模式」（观感统一）；放一个 fallback.txt 则退回「只补缺字」。
    /// 字体图集先按 charset 预热并立刻上传 GPU，避免第一帧出现方块。
    /// </summary>
    internal static class FontFix
    {
        private static readonly string[] FileNames =
        {
            "NotoSansSC-Bold.ttf", "HarmonyOS_Sans_SC_Regular.ttf", "font.ttf", "font.otf"
        };

        private static Font _unityFont;
        private static TMP_FontAsset _tmpFont;
        private static TMP_FontAsset _fallback;
        private static bool _tried;
        private static bool _replacing;
        private static bool _defaultLogged;
        private static string _folder;      // 注入模式下没有 Mod 实例，用这个目录
        private static string _fontPath;    // 注入模式：直接指定字体文件
        private static string _charsText;   // 注入模式：内存里的字符集

        public static TMP_FontAsset TmpFont { get { return _tmpFont; } }
        public static bool Replacing { get { return _replacing; } }

        public static void SetFolder(string f) { _folder = f; }
        public static void SetFontPath(string p) { _fontPath = p; }
        public static void SetCharsText(string t) { _charsText = t; }

        public static void Install()
        {
            if (_tried && _unityFont != null && _tmpFont != null) return;
            _tried = true;
            try
            {
                var manager = TranslationManager.main;
                if (manager == null || manager.fonts == null || manager.fonts.Count == 0)
                {
                    Log.Info("TranslationManager 还没准备好，等它 Awake 再装字体");
                    return;
                }
                var path = FindFontFile();
                if (path == null)
                {
                    Log.Err("没找到字体文件");
                    return;
                }
                if (_unityFont == null) _unityFont = LoadUnityFont(path);
                if (_unityFont == null)
                {
                    Log.Err("字体加载失败：" + path);
                    return;
                }
                CreateTmpFont();
                PrewarmAtlas();

                if (ReplaceMode())
                {
                    _replacing = true;
                    try
                    {
                        var global = TMP_Settings.fallbackFontAssets;
                        if (global != null && global.Count > 0 && _tmpFont != null)
                        {
                            _tmpFont.fallbackFontAssetTable = new List<TMP_FontAsset>(global);
                            Log.Info("替换模式：后备链 = 游戏 fallback（" + global.Count + " 个）");
                        }
                    }
                    catch (Exception e)
                    {
                        Log.Err("设置后备链失败：" + e.Message);
                    }
                    ReplaceNormalFont(manager);
                    ApplyToAllExisting();
                    SetDefaultFontAsset();
                    Log.Info("字体替换模式：已替换（默认）");
                }
                else
                {
                    AddAsGlobalFallback();
                    Log.Info("字体 fallback 模式：已挂到 TMP 全局 fallback");
                }
            }
            catch (Exception e)
            {
                Log.Err("Install: " + e);
            }
        }

        private static Font LoadUnityFont(string path)
        {
            try { return new Font(path); }
            catch (Exception e)
            {
                Log.Err("new Font 失败：" + e.Message);
                return null;
            }
        }

        private static void CreateTmpFont()
        {
            if (_unityFont == null || _tmpFont != null) return;
            try
            {
                // 32pt + 4096 图集 ≈ 9000+ 字形/图集；配合预热与多图集，够用且不出方块
                _tmpFont = TMP_FontAsset.CreateFontAsset(
                    _unityFont, 32, 4, GlyphRenderMode.SDFAA, 4096, 4096, AtlasPopulationMode.Dynamic, true);
                if (_tmpFont != null)
                {
                    _tmpFont.name = "SFSHan SDF";
                    try { _tmpFont.isMultiAtlasTexturesEnabled = true; } catch { }
                    _tmpFont.fallbackFontAssetTable = new List<TMP_FontAsset>();
                }
            }
            catch (Exception e)
            {
                Log.Err("CreateFontAsset: " + e.Message);
            }
        }

        private static void PrewarmAtlas()
        {
            if (_tmpFont == null) return;
            try
            {
                var all = _charsText;
                if (string.IsNullOrEmpty(all))
                {
                    var folder = Main.Instance != null ? Main.Instance.ModFolder : _folder;
                    if (string.IsNullOrEmpty(folder)) return;
                    var path = Path.Combine(folder, "chars.txt");
                    if (!File.Exists(path)) return;
                    all = File.ReadAllText(path);
                }
                var chars = new List<char>();
                // ASCII/Latin-1 也要预热：数字、字母、标点同样走动态图集
                for (char c = (char)0x20; c <= (char)0x7E; c++) chars.Add(c);
                for (char c = (char)0xA0; c <= (char)0xFF; c++) chars.Add(c);
                foreach (char c in all) if (c > 0x7F && !chars.Contains(c)) chars.Add(c);
                if (chars.Count == 0) return;

                int added = 0;
                var missing = new System.Text.StringBuilder();
                for (int i = 0; i < chars.Count; i += 500)
                {
                    var chunk = new string(chars.GetRange(i, Math.Min(500, chars.Count - i)).ToArray());
                    string miss;
                    if (_tmpFont.TryAddCharacters(chunk, out miss))
                    {
                        added += chunk.Length;
                    }
                    else
                    {
                        added += chunk.Length - (miss != null ? miss.Length : 0);
                        if (miss != null) missing.Append(miss);
                    }
                }
                Log.Info("图集预热：候选 " + chars.Count + " 字，加进 " + added +
                         (missing.Length > 0 ? "，失败 " + missing.Length + " 字" : ""));

                try
                {
                    if (_tmpFont.atlasTextures != null)
                    {
                        int up = 0;
                        foreach (var tex in _tmpFont.atlasTextures)
                        {
                            if (tex != null) { tex.Apply(false, false); ++up; }
                        }
                        Log.Info("图集贴图已上传：" + up + " 张");
                    }
                }
                catch (Exception e)
                {
                    Log.Err("上传图集失败：" + e.Message);
                }
            }
            catch (Exception e)
            {
                Log.Err("PrewarmAtlas: " + e.Message);
            }
        }

        // 有 fallback.txt 就退回「不替换游戏字体，只补缺字」
        private static bool ReplaceMode()
        {
            try
            {
                var folder = Main.Instance != null ? Main.Instance.ModFolder : _folder;
                if (string.IsNullOrEmpty(folder)) return true;
                return !File.Exists(Path.Combine(folder, "fallback.txt"));
            }
            catch { return true; }
        }

        private static void AddAsGlobalFallback()
        {
            if (_tmpFont == null) return;
            try
            {
                var list = TMP_Settings.fallbackFontAssets;
                if (list == null)
                {
                    list = new List<TMP_FontAsset>();
                    TMP_Settings.fallbackFontAssets = list;
                }
                if (!list.Contains(_tmpFont)) list.Add(_tmpFont);
                Log.Info("已加入 TMP 全局 fallback（共 " + list.Count + " 个）");
            }
            catch (Exception e)
            {
                Log.Err("AddAsGlobalFallback: " + e.Message);
            }
        }

        private static void ReplaceNormalFont(TranslationManager manager)
        {
            if (_unityFont == null || manager == null || manager.fonts == null) return;
            try
            {
                for (int i = 0; i < manager.fonts.Count; ++i) manager.fonts[i] = _unityFont;
                Log.Info("已替换 TranslationManager 字体（" + manager.fonts.Count + " 个）");
            }
            catch (Exception e)
            {
                Log.Err("ReplaceNormalFont: " + e.Message);
            }
        }

        private static string FindFontFile()
        {
            if (!string.IsNullOrEmpty(_fontPath) && File.Exists(_fontPath)) return _fontPath;
            string folder = Main.Instance != null ? Main.Instance.ModFolder : _folder;
            if (string.IsNullOrEmpty(folder))
                folder = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Mods", "SFSHanRuntime");
            foreach (var name in FileNames)
            {
                var p = Path.Combine(folder, name);
                if (File.Exists(p)) return p;
            }
            try
            {
                var any = Directory.GetFiles(folder, "*.ttf");
                if (any.Length > 0) return any[0];
                any = Directory.GetFiles(folder, "*.otf");
                if (any.Length > 0) return any[0];
            }
            catch { }
            return null;
        }

        // 文本一被创建/赋值就立刻换字体，避免「先显示方块、过一会儿才变正常」
        public static void EnsureFont(TMP_Text text)
        {
            if (!_replacing || _tmpFont == null || text == null) return;
            try { if (text.font != _tmpFont) text.font = _tmpFont; } catch { }
        }

        public static void ApplyTo(TMP_Text text)
        {
            if (text == null || _tmpFont == null) return;
            try
            {
                if (text.font != null && text.font != _tmpFont && _fallback == null)
                {
                    _fallback = text.font;
                    if (_replacing) _tmpFont.fallbackFontAssetTable = new List<TMP_FontAsset> { _fallback };
                }
                if (text.font != _tmpFont) text.font = _tmpFont;
            }
            catch { }
        }

        private static void ApplyToAllExisting()
        {
            if (_tmpFont == null) return;
            try
            {
                var all = Resources.FindObjectsOfTypeAll<TextMeshProUGUI>();
                int n = 0;
                foreach (var t in all)
                {
                    if (t == null) continue;
                    if (t.font != null && t.font != _tmpFont && _fallback == null)
                    {
                        _fallback = t.font;
                        if (_replacing) _tmpFont.fallbackFontAssetTable = new List<TMP_FontAsset> { _fallback };
                    }
                    if (t.font == _tmpFont) continue;
                    t.font = _tmpFont;
                    n++;
                }
                Log.Info("已给 " + n + " 个 TMP 文本换字体");
            }
            catch (Exception e)
            {
                Log.Err("ApplyToAllExisting: " + e.Message);
            }
        }

        // 切换场景时统一再压一次：默认字体 + 现有全部文本
        public static void ReapplyAll()
        {
            if (!_replacing) return;
            SetDefaultFontAsset();
            ApplyToAllExisting();
        }

        // TMP 的 defaultFontAsset 是只读属性，真正的字段是 TMP_Settings.m_defaultFontAsset
        public static void SetDefaultFontAsset()
        {
            if (_tmpFont == null) return;
            try
            {
                var t = typeof(TMP_Settings);
                var inst = TMP_Settings.instance;
                if (inst == null)
                {
                    try { TMP_Settings.LoadDefaultSettings(); inst = TMP_Settings.instance; } catch { }
                }
                if (inst == null) return;
                var fld = t.GetField("m_defaultFontAsset",
                    System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance |
                    System.Reflection.BindingFlags.Public);
                if (fld != null)
                {
                    fld.SetValue(inst, _tmpFont);
                    if (!_defaultLogged)
                    {
                        _defaultLogged = true;
                        Log.Info("TMP 默认字体已指向我们的字体");
                    }
                    return;
                }
                var prop = t.GetProperty("defaultFontAsset",
                    System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.Static);
                if (prop != null && prop.CanWrite) prop.SetValue(null, _tmpFont);
            }
            catch (Exception e)
            {
                Log.Err("SetDefaultFontAsset: " + e.Message);
            }
        }
    }
}
