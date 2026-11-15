// Minimal reproducible Mono Player A/B: run Baseline, copy SDK + runtime probe, then WithSdk.
using System;
using System.IO;
using System.Linq;
using UnityEditor;
using UnityEditor.Build.Reporting;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

public static class PackageSizeProbe {
    [Serializable] private class Item { public string path; public long bytes; }
    [Serializable] private class Result { public string variant; public long bytes; public Item[] files; public string[] compatiblePlugins; }
    public static void Baseline() { Build("baseline"); }
    public static void WithSdk() { Build("sdk"); }
    private static void Build(string variant) {
        PlayerSettings.SetScriptingBackend(BuildTargetGroup.Standalone, ScriptingImplementation.Mono2x);
        PlayerSettings.SetManagedStrippingLevel(BuildTargetGroup.Standalone, ManagedStrippingLevel.Disabled);
        PlayerSettings.companyName = "FengquDB"; PlayerSettings.productName = "SizeProbe";
        var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
        if (variant == "sdk") {
            var entry = Type.GetType("PackageSizeRuntime, Assembly-CSharp", true);
            new GameObject("Database probe").AddComponent(entry);
        }
        EditorSceneManager.SaveScene(scene, "Assets/Probe.unity");
        var root = Path.GetFullPath(Path.Combine(Application.dataPath, "../Player-" + variant));
        var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions {
            scenes = new[]{"Assets/Probe.unity"}, locationPathName = Path.Combine(root, "Probe.exe"),
            target = BuildTarget.StandaloneWindows64, options = BuildOptions.None
        });
        if (report.summary.result != BuildResult.Succeeded) throw new Exception("Player build failed: " + report.summary.result);
        var files = Directory.GetFiles(root, "*", SearchOption.AllDirectories)
            .Select(p => new Item{path=p.Substring(root.Length+1).Replace('\\','/'),bytes=new FileInfo(p).Length}).ToArray();
        var plugins = PluginImporter.GetAllImporters().Where(p => p.assetPath.Contains("FengquDB") && p.GetCompatibleWithPlatform(BuildTarget.StandaloneWindows64))
            .Select(p => p.assetPath).ToArray();
        File.WriteAllText(Path.Combine(root, "../" + variant + "-size.json"), JsonUtility.ToJson(new Result {
            variant=variant,bytes=files.Sum(f=>f.bytes),files=files,compatiblePlugins=plugins
        }, true));
        Debug.Log("PASS Player package size " + variant + " " + files.Sum(f=>f.bytes));
    }
}
