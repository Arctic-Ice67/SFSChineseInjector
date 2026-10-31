using System;
using System.IO;
using HarmonyLib;
using Newtonsoft.Json.Linq;

namespace SFSHanRuntime
{
    /// <summary>
    /// 纯内存注入的入口：由原生引导 DLL 通过 Mono 反射调用。
    /// 正式版不落任何文件：词表/字符集都从内存拿，日志关闭。
    /// 字体因为 Unity 的 Font 只能从路径创建，由注入器临时解到 %TEMP%，
    /// 加载完成后立刻删除（Font 已持有字体数据，删除不影响渲染）。
    /// </summary>
    public static class Injector
    {
        /// <summary>待在主线程完成的初始化（Unity API 必须主线程）</summary>
        public static bool Pending;
        private static bool _inSetup;
        private static string _fontTempPath;

        /// <summary>cfg 由注入器拼好：{dict, chars, font, fontTemp, log}</summary>
        public static void Start(string cfg)
        {
            try
            {
                var o = JObject.Parse(cfg);
                string dict = o["dict"] != null ? o["dict"].ToString() : "{}";
                string chars = o["chars"] != null ? (string)o["chars"] : "";
                string font = o["font"] != null ? (string)o["font"] : null;
                string logPath = o["log"] != null ? (string)o["log"] : null;

                Log.Init(logPath);          // logPath 为 null 时完全不写文件
                Log.Info("注入模式启动");

                Dict.LoadFromText(dict);
                FontFix.SetCharsText(chars);
                if (!string.IsNullOrEmpty(font)) FontFix.SetFontPath(font);
                _fontTempPath = (string)o["fontTemp"];

                try
                {
                    var harmony = new Harmony("com.arcticice.sfshan.inject");
                    int ok = 0, bad = 0;
                    foreach (var t in typeof(Patches).GetNestedTypes(
                                 System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic))
                    {
                        try { harmony.CreateClassProcessor(t).Patch(); ++ok; }
                        catch (Exception e) { ++bad; Log.Err("补丁 " + t.Name + " 失败：" + e.Message); }
                    }
                    Log.Info("Harmony 补丁：" + ok + " 成功 / " + bad + " 失败");
                }
                catch (Exception e)
                {
                    Log.Err("Harmony: " + e);
                }

                Pending = true;   // 主线程第一次 tick 时做字体/组件初始化
            }
            catch (Exception e)
            {
                Log.Err("Injector.Start: " + e);
            }
        }

        /// <summary>必须在 Unity 主线程调用</summary>
        public static void MainThreadSetup()
        {
            if (_inSetup) return;
            _inSetup = true;
            try
            {
                Pending = false;
                FontFix.Install();

                // 字体已经加载进 Font 对象了，临时文件可以删了
                if (!string.IsNullOrEmpty(_fontTempPath))
                {
                    try
                    {
                        if (File.Exists(_fontTempPath)) File.Delete(_fontTempPath);
                    }
                    catch { }
                    _fontTempPath = null;
                }

                try
                {
                    var go = new UnityEngine.GameObject("SFSHanRuntime_Watch");
                    UnityEngine.Object.DontDestroyOnLoad(go);
                    go.AddComponent<Watcher>();
                }
                catch (Exception e)
                {
                    Log.Err("Watcher: " + e.Message);
                }
                try
                {
                    UnityEngine.SceneManagement.SceneManager.sceneLoaded += OnSceneLoaded;
                }
                catch (Exception e)
                {
                    Log.Err("sceneLoaded: " + e.Message);
                }
                Log.Info("主线程初始化完成");
            }
            catch (Exception e)
            {
                Log.Err("MainThreadSetup: " + e);
            }
            finally
            {
                _inSetup = false;
            }
        }

        private static void OnSceneLoaded(UnityEngine.SceneManagement.Scene scene,
                                          UnityEngine.SceneManagement.LoadSceneMode mode)
        {
            try
            {
                FontFix.ReapplyAll();
            }
            catch (Exception e)
            {
                Log.Err("OnSceneLoaded: " + e.Message);
            }
        }
    }
}
