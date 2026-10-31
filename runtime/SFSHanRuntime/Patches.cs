using System;
using HarmonyLib;
using SFS.Translations;
using SFS.UI;
using TMPro;
using UnityEngine;

namespace SFSHanRuntime
{
    /// <summary>
    /// Harmony 补丁集合：
    /// ① 文本渲染时查词表替换（TMP / UGUI / IMGUI 全覆盖）
    /// ② 新出现的文本挂上我们的中文字体
    /// ③ 注入模式下提供「每帧主线程」入口，用于做 Unity 侧初始化
    /// </summary>
    internal static class Patches
    {
        // ---------- ① 字体 ----------

        // 任何代码想把字体设回游戏字体，都改写成我们的
        [HarmonyPatch(typeof(TMP_Text), "set_font")]
        internal static class TMP_SetFont
        {
            private static void Prefix(ref TMP_FontAsset value)
            {
                if (FontFix.Replacing && FontFix.TmpFont != null) value = FontFix.TmpFont;
            }
        }

        [HarmonyPatch(typeof(TextMeshProUGUI), "Awake")]
        internal static class TMPUGUI_Awake
        {
            private static void Prefix(TextMeshProUGUI __instance) { FontFix.EnsureFont(__instance); }
        }

        [HarmonyPatch(typeof(TextMeshPro), "Awake")]
        internal static class TMP3D_Awake
        {
            private static void Prefix(TextMeshPro __instance) { FontFix.EnsureFont(__instance); }
        }

        [HarmonyPatch(typeof(TextMeshProUGUI), "OnEnable")]
        internal static class TMP_OnEnable
        {
            private static void Postfix(TextMeshProUGUI __instance) { FontFix.ApplyTo(__instance); }
        }

        [HarmonyPatch(typeof(FontSetter), "SetFont")]
        internal static class FontSetter_SetFont
        {
            private static void Postfix(FontSetter __instance)
            {
                if (__instance == null || FontFix.TmpFont == null) return;
                try
                {
                    var t = __instance.GetComponentInChildren<TextMeshProUGUI>();
                    if (t != null) FontFix.ApplyTo(t);
                }
                catch { }
            }
        }

        [HarmonyPatch(typeof(TextAdapter), "Awake")]
        internal static class TextAdapter_Awake
        {
            private static void Postfix(TextAdapter __instance)
            {
                if (__instance == null) return;
                try
                {
                    var t = __instance.GetComponent<TextMeshProUGUI>();
                    if (t != null) FontFix.ApplyTo(t);
                }
                catch { }
            }
        }

        [HarmonyPatch(typeof(TranslationManager), "Awake")]
        internal static class TranslationManager_Awake
        {
            private static void Postfix() { FontFix.Install(); }
        }

        [HarmonyPatch(typeof(TranslationManager), "SetLanguage")]
        internal static class TranslationManager_SetLanguage
        {
            private static void Postfix(TranslationManager __instance)
            {
                FontFix.Install();
                if (FontFix.TmpFont == null) return;
                try
                {
                    if (__instance != null) __instance.currentFont = null;  // 让下次取字体时走我们的
                }
                catch { }
            }
        }

        // ---------- ② 每帧主线程入口 ----------

        // 注入模式：字体/组件初始化不能等「某段文本刚好被重新赋值」（静态菜单等不到）
        [HarmonyPatch(typeof(UnityEngine.EventSystems.EventSystem), "Update")]
        internal static class Tick_EventSystem
        {
            private static void Prefix()
            {
                if (Injector.Pending) Injector.MainThreadSetup();
            }
        }

        [HarmonyPatch(typeof(Canvas), "ForceUpdateCanvases")]
        internal static class Tick_Canvas
        {
            private static void Prefix()
            {
                if (Injector.Pending) Injector.MainThreadSetup();
            }
        }

        // ---------- ③ 文本翻译 ----------

        [HarmonyPatch(typeof(TMP_Text), "set_text")]
        internal static class TMP_SetText
        {
            private static void Prefix(TMP_Text __instance, ref string value)
            {
                if (Injector.Pending) Injector.MainThreadSetup();
                FontFix.EnsureFont(__instance);
                value = Dict.Translate(value);
            }
        }

        [HarmonyPatch(typeof(TMP_Text), "SetCharArray", new Type[] { typeof(char[]) })]
        internal static class TMP_SetCharArray
        {
            private static void Prefix(TMP_Text __instance, ref char[] sourceText)
            {
                FontFix.EnsureFont(__instance);
                if (sourceText == null || sourceText.Length == 0 || sourceText.Length > 4096) return;
                var s = new string(sourceText);
                var t = Dict.Translate(s);
                if (!string.Equals(s, t, StringComparison.Ordinal)) sourceText = t.ToCharArray();
            }
        }

        [HarmonyPatch(typeof(UnityEngine.UI.Text), "set_text")]
        internal static class UGUI_Text_SetText
        {
            private static void Prefix(ref string value) { value = Dict.Translate(value); }
        }

        [HarmonyPatch(typeof(TextAdapter), "set_Text")]
        internal static class TextAdapter_SetText
        {
            private static void Prefix(ref string value) { value = Dict.Translate(value); }
        }

        // IMGUI（string 重载）
        [HarmonyPatch(typeof(GUI), "Label", new Type[] { typeof(Rect), typeof(string) })]
        internal static class GUI_Label
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        [HarmonyPatch(typeof(GUI), "Label", new Type[] { typeof(Rect), typeof(string), typeof(GUIStyle) })]
        internal static class GUI_LabelStyle
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        [HarmonyPatch(typeof(GUI), "Button", new Type[] { typeof(Rect), typeof(string) })]
        internal static class GUI_Button
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        [HarmonyPatch(typeof(GUI), "Button", new Type[] { typeof(Rect), typeof(string), typeof(GUIStyle) })]
        internal static class GUI_ButtonStyle
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        [HarmonyPatch(typeof(GUI), "Box", new Type[] { typeof(Rect), typeof(string) })]
        internal static class GUI_Box
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        [HarmonyPatch(typeof(GUI), "Box", new Type[] { typeof(Rect), typeof(string), typeof(GUIStyle) })]
        internal static class GUI_BoxStyle
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        [HarmonyPatch(typeof(GUI), "Toggle", new Type[] { typeof(Rect), typeof(bool), typeof(string) })]
        internal static class GUI_Toggle
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        // IMGUI（GUIContent 重载）—— Warpinator 这类面板走的是它
        [HarmonyPatch(typeof(GUI), "Label", new Type[] { typeof(Rect), typeof(GUIContent) })]
        internal static class GUI_LabelContent
        {
            private static void Prefix(ref GUIContent content)
            {
                if (content != null) content.text = Dict.Translate(content.text);
            }
        }

        [HarmonyPatch(typeof(GUI), "Button", new Type[] { typeof(Rect), typeof(GUIContent) })]
        internal static class GUI_ButtonContent
        {
            private static void Prefix(ref GUIContent content)
            {
                if (content != null) content.text = Dict.Translate(content.text);
            }
        }

        [HarmonyPatch(typeof(GUI), "Box", new Type[] { typeof(Rect), typeof(GUIContent) })]
        internal static class GUI_BoxContent
        {
            private static void Prefix(ref GUIContent content)
            {
                if (content != null) content.text = Dict.Translate(content.text);
            }
        }

        // GUILayout（自动布局面板）
        [HarmonyPatch(typeof(GUILayout), "Label", new Type[] { typeof(string), typeof(GUILayoutOption[]) })]
        internal static class GUILayout_Label
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        [HarmonyPatch(typeof(GUILayout), "Button", new Type[] { typeof(string), typeof(GUILayoutOption[]) })]
        internal static class GUILayout_Button
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }

        [HarmonyPatch(typeof(GUILayout), "Box", new Type[] { typeof(string), typeof(GUILayoutOption[]) })]
        internal static class GUILayout_Box
        {
            private static void Prefix(ref string text) { text = Dict.Translate(text); }
        }
    }

    /// <summary>每 2 秒看一次 dict.json（热更新），另外每 0.6 秒兜底清扫一遍所有文本。</summary>
    internal class Watcher : MonoBehaviour
    {
        private float _next;
        private float _nextSweep;
        private static bool _firstDone;

        private void Update()
        {
            if (Time.unscaledTime < _next) return;
            _next = Time.unscaledTime + 2f;
            Dict.Poll();
            if (Dict.WasReloaded()) Log.Info("词表热更新生效：" + Dict.Count + " 条");

            if (!_firstDone && Time.unscaledTime > 1f)
            {
                _firstDone = true;
                FontFix.ReapplyAll();
                Log.Info("启动后首次字体重压完成");
            }
            if (Time.unscaledTime < _nextSweep) return;
            _nextSweep = Time.unscaledTime + 0.6f;
            Sweep();
        }

        private static void Sweep()
        {
            try
            {
                int n = 0;
                var tmps = Resources.FindObjectsOfTypeAll<TMP_Text>();
                foreach (var t in tmps)
                {
                    if (t == null) continue;
                    // 替换模式下：新创建的文本对象还在用游戏原字体（它是残缺子集，会出方块），一起换掉
                    if (FontFix.Replacing && FontFix.TmpFont != null && t.font != FontFix.TmpFont)
                    {
                        t.font = FontFix.TmpFont;
                        ++n;
                    }
                    if (string.IsNullOrEmpty(t.text) || t.text.Length > 200) continue;
                    var tr = Dict.Translate(t.text);
                    if (!string.Equals(tr, t.text, StringComparison.Ordinal))
                    {
                        t.text = tr;
                        ++n;
                    }
                }
                var ugui = Resources.FindObjectsOfTypeAll<UnityEngine.UI.Text>();
                foreach (var t in ugui)
                {
                    if (t == null || string.IsNullOrEmpty(t.text) || t.text.Length > 200) continue;
                    var tr = Dict.Translate(t.text);
                    if (!string.Equals(tr, t.text, StringComparison.Ordinal))
                    {
                        t.text = tr;
                        ++n;
                    }
                }
                if (n > 0) Log.Info("兜底清扫：改了 " + n + " 处文本");
            }
            catch (Exception e)
            {
                Log.Err("Sweep: " + e.Message);
            }
        }
    }
}
