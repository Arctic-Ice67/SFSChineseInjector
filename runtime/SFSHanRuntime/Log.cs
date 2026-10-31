using System;
using System.IO;
using UnityEngine;

namespace SFSHanRuntime
{
    /// <summary>落盘日志（Mods\SFSHanRuntime\SFSHanRuntime.log），出问题先看这个。</summary>
    internal static class Log
    {
        private static string _path;
        private static bool _ready;

        public static void Init(string modFolder)
        {
            try
            {
                if (string.IsNullOrEmpty(modFolder))   // 单文件版：不写任何日志
                {
                    _path = null;
                    _ready = false;
                    return;
                }
                if (string.IsNullOrEmpty(modFolder))
                    modFolder = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Mods", "SFSHanRuntime");
                Directory.CreateDirectory(modFolder);
                _path = Path.Combine(modFolder, "SFSHanRuntime.log");
                File.WriteAllText(_path, "=== SFSHanRuntime " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " ===\n");
                _ready = true;
            }
            catch
            {
                _ready = false;
            }
        }

        public static void Info(string msg)
        {
            Write("INFO  " + msg);
        }

        public static void Err(string msg)
        {
            Write("ERROR " + msg);
            try { Debug.LogError("[SFSHan] " + msg); } catch { }
        }

        private static void Write(string line)
        {
            try { Debug.Log("[SFSHan] " + line); } catch { }
            if (!_ready) return;
            try { File.AppendAllText(_path, DateTime.Now.ToString("HH:mm:ss.fff") + " " + line + "\n"); }
            catch { }
        }
    }
}
