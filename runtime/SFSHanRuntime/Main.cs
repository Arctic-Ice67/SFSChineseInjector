using System;
using HarmonyLib;
using ModLoader;
using UnityEngine;

namespace SFSHanRuntime
{
    public class Main : Mod
    {
        public static Main Instance;

        public override string ModNameID { get { return "sfs_han_runtime"; } }
        public override string DisplayName { get { return "SFS 汉化运行时"; } }
        public override string Author { get { return "Ice"; } }
        public override string ModVersion { get { return "0.1.0"; } }
        public override string MinimumGameVersionNecessary { get { return "1.5.9"; } }
        public override string Description
        {
            get
            {
                return "中文字体 + 运行时词表翻译。词表放在本 mod 目录的 dict.json，改完不用重启游戏。";
            }
        }

        public override void Early_Load()
        {
            Instance = this;
            try
            {
                var harmony = new Harmony("com.arcticice.sfshan");
                foreach (var t in typeof(Patches).GetNestedTypes(System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic))
                {
                    try { harmony.CreateClassProcessor(t).Patch(); }
                    catch (Exception e) { Debug.LogError("[SFSHan] 补丁 " + t.Name + " 失败: " + e.Message); }
                }
            }
            catch (Exception e)
            {
                Debug.LogError("[SFSHan] Harmony PatchAll 失败: " + e);
            }
        }

        public override void Load()
        {
            Log.Init(ModFolder);
            Log.Info("starting " + ModVersion);
            Dict.Load(ModFolder);
            FontFix.Install();
            try
            {
                var go = new GameObject("SFSHanRuntime_Watch");
                UnityEngine.Object.DontDestroyOnLoad(go);
                go.AddComponent<Watcher>();
            }
            catch (Exception e)
            {
                Log.Err("Watcher: " + e.Message);
            }

            // 切换场景时立刻重压一遍字体：新场景的 UI 是「刚创建」的，
            // 如果等清扫器（0.6 秒）就会看到一瞬方块。
            try
            {
                UnityEngine.SceneManagement.SceneManager.sceneLoaded += OnSceneLoaded;
                Log.Info("已挂场景切换回调");
            }
            catch (Exception e)
            {
                Log.Err("sceneLoaded: " + e.Message);
            }
            Log.Info("loaded v" + ModVersion + " dict=" + Dict.Count + " prefix=" + Dict.PrefixCount);
        }

        private static void OnSceneLoaded(UnityEngine.SceneManagement.Scene scene,
                                          UnityEngine.SceneManagement.LoadSceneMode mode)
        {
            try
            {
                FontFix.ReapplyAll();
                Log.Info("场景已加载，字体重压：" + scene.name);
            }
            catch (Exception e)
            {
                Log.Err("OnSceneLoaded: " + e.Message);
            }
        }
    }
}
