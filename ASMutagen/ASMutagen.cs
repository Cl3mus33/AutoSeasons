namespace ASMutagen;

// Base Imports
using System;
using System.Runtime.InteropServices;
using System.Runtime.CompilerServices;

// Mutagen
using Mutagen.Bethesda;
using Mutagen.Bethesda.Skyrim;
using Mutagen.Bethesda.Environments;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Noggog;

using Mutagen.Bethesda.Plugins.Exceptions;
using Mutagen.Bethesda.Strings;
using Mutagen.Bethesda.Plugins.Binary.Streams;
using Mutagen.Bethesda.Strings.DI;
using Mutagen.Bethesda.Plugins.Aspects;

using Google.FlatBuffers;
using System.Collections;

public static class ExceptionHandler
{
    private static string? LastExceptionMessage;

    public static void SetLastException(Exception? ex)
    {
        // Find the deepest inner exception
        while (ex is not null)
        {
            LastExceptionMessage += "\n" + ex.Message + "\n" + ex.StackTrace;
            ex = ex.InnerException;
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "GetLastException", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void GetLastException([DNNE.C99Type("wchar_t**")] IntPtr* errorMessagePtr)
    {
        string? errorMessage = LastExceptionMessage ?? string.Empty;

        if (LastExceptionMessage.IsNullOrEmpty())
        {
            return;
        }

        *errorMessagePtr = Marshal.StringToHGlobalUni(errorMessage);
    }
}

public static class MessageHandler
{
    private static Queue<Tuple<string, int>> LogQueue = [];

    // structures to keep track of warning and error messages to avoid duplicates
    private static readonly HashSet<string> WarningMessages = [];
    private static readonly HashSet<string> ErrorMessages = [];

    public static void Log(string message, int level = 0)
    {
        // Level values
        // 0: Trace
        // 1: Debug
        // 2: Info
        // 3: Warning
        // 4: Error
        // 5: Critical

        if (level == 3)
        {
            // Warning level, check for duplicates
            if (WarningMessages.Contains(message))
            {
                return;
            }
            WarningMessages.Add(message);
        }
        else if (level == 4)
        {
            // Error level, check for duplicates
            if (ErrorMessages.Contains(message))
            {
                return;
            }
            ErrorMessages.Add(message);
        }

        LogQueue.Enqueue(new Tuple<string, int>(message, level));
    }

    [UnmanagedCallersOnly(EntryPoint = "GetLogMessage", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void GetLogMessage([DNNE.C99Type("wchar_t**")] IntPtr* messagePtr, [DNNE.C99Type("int*")] int* level)
    {
        if (LogQueue.Count == 0)
        {
            return;
        }

        var logMessage = LogQueue.Dequeue();
        string message = logMessage.Item1;
        *messagePtr = Marshal.StringToHGlobalUni(logMessage.Item1);
        *level = logMessage.Item2;
    }
}

class StructuralArrayComparer : IEqualityComparer<string[]>
{
    public bool Equals(string[]? x, string[]? y)
    {
        return StructuralComparisons.StructuralEqualityComparer.Equals(x, y);
    }

    public int GetHashCode(string[] obj)
    {
        return StructuralComparisons.StructuralEqualityComparer.GetHashCode(obj);
    }
}

public class ASMutagen
{
    //
    // Class Members
    //

    // The single output plugin: holds every seasonal STAT/LTEX duplicate record plus the TXSTs
    // they reference. Not an override plugin - every record in it is genuinely new - so it must
    // only be ESL-flagged when its own FormIDs actually fit the light-plugin range (checked at
    // Finalize time), never unconditionally.
    private static SkyrimMod? SeasonsMod;
    private static IGameEnvironment<ISkyrimMod, ISkyrimModGetter>? Env;
    private static Dictionary<string, List<Tuple<FormKey, string>>> ModelUses = [];
    private static Dictionary<string[], ITextureSet> NewTextureSets = new(new StructuralArrayComparer());
    private static SortedSet<uint> allocatedFormIDs = [];
    private static uint lastUsedFormID = 1;

    // Baseline captured immediately after PopulateObjs() to support rerun resets.
    private static SortedSet<uint> InitialAllocatedFormIDs = [];
    private static uint InitialLastUsedFormID = 1;

    private static SkyrimRelease GameType;
    private static Language PluginLanguage = Language.English;

    private class Utf8EncodingWrapper : IMutagenEncodingProvider
    {
        public IMutagenEncoding GetEncoding(GameRelease release, Language language)
        {
            return MutagenEncoding._utf8;
        }
    }

    //
    // Public API
    //

    [UnmanagedCallersOnly(EntryPoint = "Initialize", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void Initialize(
      [DNNE.C99Type("const int")] int gameType,
      [DNNE.C99Type("const wchar_t*")] IntPtr exePath,
      [DNNE.C99Type("const wchar_t*")] IntPtr dataPathPtr,
      [DNNE.C99Type("const wchar_t**")] IntPtr* loadOrder,
      [DNNE.C99Type("const unsigned int")] uint pluginLang)
    {
        try
        {
            // Set base directory so Mutagen can resolve assemblies relative to our DLL location
            string ExePath = Marshal.PtrToStringUni(exePath) ?? string.Empty;
            AppContext.SetData("APP_CONTEXT_BASE_DIRECTORY", ExePath);

            // Main method
            string dataPath = Marshal.PtrToStringUni(dataPathPtr) ?? string.Empty;
            MessageHandler.Log("[Initialize] Data Path: " + dataPath, 0);

            PluginLanguage = (Language)pluginLang;
            GameType = (SkyrimRelease)gameType;
            SeasonsMod = new SkyrimMod(ModKey.FromFileName("AutoSeasons.esp"), GameType);

            // First, convert the pointers to proper strings
            List<ModKey> loadOrderList = [];
            for (int i = 0; loadOrder[i] != IntPtr.Zero; i++)
            {
                var curModName = Marshal.PtrToStringUni(loadOrder[i]) ?? string.Empty;
                if (curModName.IsNullOrEmpty())
                {
                    continue;
                }

                loadOrderList.Add(ModKey.FromFileName(curModName));
            }

            // Stores mods that have missing masters
            HashSet<ModKey> modsWithMissingMasters = [];

            // Create validMods to indicate all mods which actually exist and are loadable
            HashSet<ModKey> validMods = [];
            foreach (var modKey in loadOrderList)
            {
                string modPath = Path.Combine(dataPath, modKey.FileName);
                if (File.Exists(modPath))
                {
                    validMods.Add(modKey);
                }
            }

            // Loop through loadOrderList and determine if any mods have missing masters
            List<ModKey> finalLoadOrderList = [];
            foreach (var modKey in loadOrderList)
            {
                if (!validMods.Contains(modKey))
                {
                    // skip mods that don't exist
                    continue;
                }

                string modPath = Path.Combine(dataPath, modKey.FileName);
                ISkyrimModDisposableGetter? curMod = null;
                try
                {
                    curMod = SkyrimMod.Create(GameType).FromPath(modPath).Construct();
                }
                catch (Exception)
                {
                    // failed to load mod, skip
                    MessageHandler.Log("Failed to read plugin: " + modKey.FileName, 4);

                    // add to missing masters
                    modsWithMissingMasters.Add(modKey);

                    continue;
                }
                // loop through masters
                bool currentModHasMissingMasters = false;
                for (int j = 0; j < curMod.MasterReferences.Count; j++)
                {
                    var masterModKey = curMod.MasterReferences[j].Master;
                    if (!validMods.Contains(masterModKey) || modsWithMissingMasters.Contains(masterModKey))
                    {
                        modsWithMissingMasters.Add(masterModKey);
                        currentModHasMissingMasters = true;
                    }
                }

                if (!currentModHasMissingMasters)
                {
                    finalLoadOrderList.Add(modKey);
                }
                else
                {
                    modsWithMissingMasters.Add(modKey);
                    MessageHandler.Log("Plugin has missing masters: " + modKey.FileName, 4);
                }
            }

            var stringReadParams = new StringsReadParameters()
            {
                TargetLanguage = PluginLanguage,
                EncodingProvider = new Utf8EncodingWrapper()
            };

            try
            {
                Env = GameEnvironment.Typical.Builder<ISkyrimMod, ISkyrimModGetter>((GameRelease)GameType)
                    .WithTargetDataFolder(dataPath)
                    .WithLoadOrder(finalLoadOrderList.ToArray())
                    .WithOutputMod(SeasonsMod)
                    .WithStringParameters(stringReadParams)
                    .Build();
            }
            catch (Exception ex)
            {
                MessageHandler.Log("Failed to build plugin environment. This is usually due to a bad plugin in your load order. Message: " + ex.Message, 5);
            }
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "PopulateObjs", CallConvs = [typeof(CallConvCdecl)])]
    public static void PopulateObjs([DNNE.C99Type("const wchar_t*")] IntPtr oldPGPluginPath)
    {
        try
        {
            if (Env is null || SeasonsMod is null)
            {
                throw new Exception("Initialize must be called before PopulateObjs");
            }

            foreach (var modelMajorRec in EnumerateModelRecordsSafe())
            {
                if (modelMajorRec.FormKey.ModKey == ModKey.Null)
                {
                    // null modkey, this is invalid and we should skip
                    continue;
                }

                // Will store models to check later
                var ModelRecs = GetModelElems(modelMajorRec);

                if (ModelRecs.Count == 0)
                {
                    // Skip if there are no MODL records in this record
                    continue;
                }

                foreach (var modelRec in ModelRecs)
                {
                    // add to model uses
                    string meshName;
                    try
                    {
                        if (modelRec.Item1.File.IsNullOrEmpty())
                        {
                            continue;
                        }
                        meshName = modelRec.Item1.File.ToString().ToLowerInvariant();
                    }
                    catch (Exception)
                    {
                        MessageHandler.Log("Unable to read model path: " + GetRecordDesc(modelMajorRec), 3);
                        continue;
                    }

                    var curTuple = new Tuple<FormKey, string>(modelMajorRec.FormKey, modelRec.Item2);

                    // Check if we need to also add the weight counterpart
                    bool isWeighted = false;
                    if (modelMajorRec is IArmorAddonGetter armorAddonGetter)
                    {
                        if (((modelRec.Item2 == "MALE" || modelRec.Item2 == "1STMALE") && armorAddonGetter.WeightSliderEnabled.Male) || ((modelRec.Item2 == "FEMALE" || modelRec.Item2 == "1STFEMALE") && armorAddonGetter.WeightSliderEnabled.Female))
                        {
                            isWeighted = true;
                        }
                    }
                    if (isWeighted)
                    {
                        // weight slider is enabled
                        string weightMeshName = string.Empty;
                        if (meshName.EndsWith("_0.nif"))
                        {
                            // replace _0.nif with _1.nif
                            weightMeshName = string.Concat(meshName.AsSpan(0, meshName.Length - 6), "_1.nif");
                        }
                        else if (meshName.EndsWith("_1.nif"))
                        {
                            // replace _1.nif with _0.nif
                            weightMeshName = string.Concat(meshName.AsSpan(0, meshName.Length - 6), "_0.nif");
                        }
                        else
                        {
                            MessageHandler.Log("Weight slider enabled but mesh name doesn't end with _0.nif or _1.nif: " + GetRecordDesc(modelMajorRec), 3);
                            continue;
                        }

                        if (!weightMeshName.IsNullOrEmpty())
                        {
                            // Add weighted model use
                            if (!ModelUses.ContainsKey(weightMeshName))
                            {
                                ModelUses[weightMeshName] = [];
                            }

                            ModelUses[weightMeshName].Add(curTuple);
                        }
                    }

                    // Add regular model use
                    if (!ModelUses.ContainsKey(meshName))
                    {
                        ModelUses[meshName] = [];
                    }

                    ModelUses[meshName].Add(curTuple);
                }
            }

            // Capture baseline immediately after PopulateObjs so reruns can reset to this state.
            InitialAllocatedFormIDs = [.. allocatedFormIDs];
            InitialLastUsedFormID = lastUsedFormID;
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "ResetPatchingState", CallConvs = [typeof(CallConvCdecl)])]
    public static void ResetPatchingState([DNNE.C99Type("const int")] int _reserved)
    {
        try
        {
            if (Env is null || SeasonsMod is null)
            {
                throw new Exception("Initialize must be called before ResetPatchingState");
            }

            // Restore allocation state from the post-PopulateObjs baseline.
            allocatedFormIDs = [.. InitialAllocatedFormIDs];
            lastUsedFormID = InitialLastUsedFormID;

            SeasonsMod = new SkyrimMod(ModKey.FromFileName("AutoSeasons.esp"), GameType);
            NewTextureSets = new Dictionary<string[], ITextureSet>(new StructuralArrayComparer());
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    // esmMode: 0/1 = ESM-flag the output plugin, 2 = do not ESM-flag it
    [UnmanagedCallersOnly(EntryPoint = "Finalize", CallConvs = [typeof(CallConvCdecl)])]
    public static void Finalize([DNNE.C99Type("const wchar_t*")] IntPtr outputPathPtr, [DNNE.C99Type("const int")] int esmMode)
    {
        try
        {
            if (Env is null || SeasonsMod is null)
            {
                throw new Exception("Initialize must be called before Finalize");
            }

            if (!SeasonsMod.EnumerateMajorRecords().Any())
            {
                // No records to write, return early
                return;
            }

            string outputPath = Marshal.PtrToStringUni(outputPathPtr) ?? string.Empty;

            // Only ESL-flag if the new records actually fit the light-plugin FormID range - this
            // plugin holds genuinely new records (not overrides), so it must not be unconditionally
            // ESL-flagged the way an override-only plugin safely could be.
            uint seasonsMaxFormID = 0;
            foreach (var rec in SeasonsMod.EnumerateMajorRecords())
            {
                seasonsMaxFormID = Math.Max(seasonsMaxFormID, rec.FormKey.ID);
            }

            if (seasonsMaxFormID <= 0xFFF)
            {
                SeasonsMod.IsSmallMaster = true;
            }
            if (esmMode != 2)
            {
                SeasonsMod.IsMaster = true;
            }

            // Reads the FINAL merged load order directly (whatever mods/overrides are actually
            // winning right now, including PGPatcher's own output if the user has it installed) -
            // no special-cased "duplicate from patched-in-memory-record" bridging needed, since
            // there's no in-process patching pass to coordinate with.
            SeasonsMod.BeginWrite
                .ToPath(Path.Combine(outputPath, SeasonsMod.ModKey.FileName))
                .WithLoadOrder(Env.LoadOrder)
                .NoFormIDCompactnessCheck()
                .Write();
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    [UnmanagedCallersOnly(EntryPoint = "GetModelUses", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void GetModelUses(
      [DNNE.C99Type("const wchar_t*")] IntPtr modelPathPtr,
      [DNNE.C99Type("unsigned int*")] uint* length,
      [DNNE.C99Type("uint8_t**")] byte** bufferPtr)
    {
        try
        {
            if (Env is null)
            {
                throw new Exception("Initialize must be called before GetModelUses");
            }

            if (length is null)
            {
                throw new Exception("length pointer is null");
            }

            var builder = new FlatBufferBuilder(1024);

            // Get the lowercase nifname (with meshes\ prefix) from C++
            string nifName = Marshal.PtrToStringUni(modelPathPtr)?.ToLowerInvariant() ?? string.Empty;

            // find all uses
            if (!ModelUses.TryGetValue(nifName, out List<Tuple<FormKey, string>>? modelRecUsesList))
            {
                // No uses for this model
                *length = 0;
                return;
            }

            var modelUseOffsets = new List<Offset<ASMutagenBuffers.ModelUse>>();

            // loop through each use
            for (int i = 0; i < modelRecUsesList.Count; i++)
            {
                var formKey = modelRecUsesList[i].Item1;
                var subModel = modelRecUsesList[i].Item2;

                // Try to resolve the model record and submodel
                if (!Env.LinkCache.TryResolve<IMajorRecordGetter>(formKey, out var modelRec) ||
                    GetModelElemBySubModel(modelRec, subModel) is not { } matchedModel)
                {
                    MessageHandler.Log($"Failed to resolve model record: {GetRecordDesc(formKey)}", 4);
                    continue;
                }

                var altTexVector = new VectorOffset();
                if (matchedModel.AlternateTextures is not null)
                {
                    var altTexOffsets = new List<Offset<ASMutagenBuffers.AlternateTexture>>();

                    for (int j = 0; j < matchedModel.AlternateTextures.Count; j++)
                    {
                        var altTexIdx = matchedModel.AlternateTextures[j].Index;
                        var newTXST = matchedModel.AlternateTextures[j].NewTexture;

                        var textureSetOffsets = new List<Offset<ASMutagenBuffers.TextureSet>>();

                        // find newTXST record
                        var textures = new string[8];
                        if (Env.LinkCache.TryResolve<ITextureSetGetter>(newTXST.FormKey, out var newTXSTRec))
                        {
                            // The 8 strings in textureset are:
                            // newTXSTRec.Texture1 - Texture8
                            textures = GetTextureSet(newTXSTRec);
                            for (int k = 0; k < 8; k++)
                            {
                                if (textures[k].IsNullOrEmpty())
                                {
                                    continue;
                                }

                                textures[k] = AddPrefixIfNotExists("textures\\", textures[k]);
                            }
                        }
                        else
                        {
                            // the 8 strings in the textureset should exist but all be empty
                            textures = [.. Enumerable.Repeat(string.Empty, 8)];
                        }

                        // Build the TextureSet
                        var textureSetVec = ASMutagenBuffers.TextureSet.CreateTexturesVector(
                            builder,
                            [.. textures.Select(s => builder.CreateString(s))]
                        );

                        ASMutagenBuffers.TextureSet.StartTextureSet(builder);
                        ASMutagenBuffers.TextureSet.AddTextures(builder, textureSetVec);
                        var textureSetOffset = ASMutagenBuffers.TextureSet.EndTextureSet(builder);

                        // Build the AlternateTexture (slots now holds a single TextureSet, not a vector)
                        ASMutagenBuffers.AlternateTexture.StartAlternateTexture(builder);
                        ASMutagenBuffers.AlternateTexture.AddSlotId(builder, altTexIdx);
                        // add slot_id_new if you have a value for it
                        ASMutagenBuffers.AlternateTexture.AddSlots(builder, textureSetOffset);
                        var altTexOffset = ASMutagenBuffers.AlternateTexture.EndAlternateTexture(builder);

                        altTexOffsets.Add(altTexOffset);
                    }

                    altTexVector = ASMutagenBuffers.ModelUse.CreateAlternateTexturesVector(builder, [.. altTexOffsets]);
                }
                else
                {
                    altTexVector = ASMutagenBuffers.ModelUse.CreateAlternateTexturesVector(builder, []);
                }

                // check if this is IStaticGetter for materials
                bool is_singlePass = false;
                if (modelRec is IStaticGetter staticRec && Env.LinkCache.TryResolve<IMaterialObjectGetter>(staticRec.Material.FormKey, out var materialRec))
                {
                    is_singlePass = (materialRec.Flags & MaterialObject.Flag.SinglePass) != 0;
                }

                bool is_weighted = false;
                if (modelRec is IArmorAddonGetter armorAddonRec)
                {
                    if (((subModel == "MALE" || subModel == "1STMALE") && armorAddonRec.WeightSliderEnabled.Male) || ((subModel == "FEMALE" || subModel == "1STFEMALE") && armorAddonRec.WeightSliderEnabled.Female))
                    {
                        is_weighted = true;
                    }
                }

                // Record flag 24
                bool is_ignored = (modelRec.MajorRecordFlagsRaw & 0x01000000) != 0;

                string recType = GetXEditTypeFromType(modelRec);

                var modNameOffset = builder.CreateString(formKey.ModKey.FileName);
                var subModelOffset = builder.CreateString(subModel);
                var modelNameOffset = builder.CreateString(nifName);
                var recTypeOffset = builder.CreateString(recType);
                var editorIdOffset = builder.CreateString(modelRec.EditorID ?? string.Empty);

                ASMutagenBuffers.ModelUse.StartModelUse(builder);
                ASMutagenBuffers.ModelUse.AddModName(builder, modNameOffset);
                ASMutagenBuffers.ModelUse.AddFormId(builder, formKey.ID);
                ASMutagenBuffers.ModelUse.AddSubModel(builder, subModelOffset);
                ASMutagenBuffers.ModelUse.AddIsWeighted(builder, is_weighted);
                ASMutagenBuffers.ModelUse.AddMeshFile(builder, modelNameOffset);
                ASMutagenBuffers.ModelUse.AddSinglepassMato(builder, is_singlePass);
                ASMutagenBuffers.ModelUse.AddIsIgnored(builder, is_ignored);
                ASMutagenBuffers.ModelUse.AddType(builder, recTypeOffset);
                ASMutagenBuffers.ModelUse.AddAlternateTextures(builder, altTexVector);
                ASMutagenBuffers.ModelUse.AddEditorId(builder, editorIdOffset);
                var modelUseOffset = ASMutagenBuffers.ModelUse.EndModelUse(builder);

                modelUseOffsets.Add(modelUseOffset);
            }

            var usesVector = ASMutagenBuffers.ModelUses.CreateUsesVector(builder, [.. modelUseOffsets]);
            ASMutagenBuffers.ModelUses.StartModelUses(builder);
            ASMutagenBuffers.ModelUses.AddUses(builder, usesVector);
            var rootOffset = ASMutagenBuffers.ModelUses.EndModelUses(builder);
            builder.Finish(rootOffset.Value);

            var byteArray = builder.SizedByteArray(); // fully serialized FlatBuffer
            *length = (uint)byteArray.Length;

            // Allocate memory for C++ side
            *bufferPtr = (byte*)Marshal.AllocHGlobal(byteArray.Length);
            Marshal.Copy(byteArray, 0, (IntPtr)(*bufferPtr), byteArray.Length);
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    // Returns every LTEX record's current diffuse texture, for the C++ side to check for a
    // seasonal sibling on disk. LandscapeTexture has a single TextureSet FormLink (no
    // alternate-texture system), so this is simpler than GetModelUses.
    [UnmanagedCallersOnly(EntryPoint = "EnumerateLandscapeTextures", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void EnumerateLandscapeTextures(
      [DNNE.C99Type("unsigned int*")] uint* outLength,
      [DNNE.C99Type("uint8_t**")] byte** outBufferPtr)
    {
        try
        {
            if (Env is null)
            {
                throw new Exception("Initialize must be called before EnumerateLandscapeTextures");
            }

            if (outLength is null)
            {
                throw new Exception("outLength pointer is null");
            }

            var builder = new FlatBufferBuilder(1024);
            var entryOffsets = new List<Offset<ASMutagenBuffers.LandscapeTextureEntry>>();

            foreach (var ltex in Env.LoadOrder.PriorityOrder.LandscapeTexture().WinningOverrides())
            {
                string[] slots = [.. Enumerable.Repeat(string.Empty, 8)];
                if (ltex.TextureSet.TryResolve(Env.LinkCache, out var txst))
                {
                    slots = GetTextureSet(txst);
                    for (int k = 0; k < 8; k++)
                    {
                        if (!slots[k].IsNullOrEmpty())
                        {
                            slots[k] = AddPrefixIfNotExists("textures\\", slots[k]);
                        }
                    }
                }

                var modNameOffset = builder.CreateString(ltex.FormKey.ModKey.FileName);
                var editorIdOffset = builder.CreateString(ltex.EditorID ?? string.Empty);
                var slotsVec = ASMutagenBuffers.TextureSet.CreateTexturesVector(
                    builder,
                    [.. slots.Select(s => builder.CreateString(s))]
                );
                ASMutagenBuffers.TextureSet.StartTextureSet(builder);
                ASMutagenBuffers.TextureSet.AddTextures(builder, slotsVec);
                var slotsOffset = ASMutagenBuffers.TextureSet.EndTextureSet(builder);

                var grassOffsets = new List<Offset<ASMutagenBuffers.FormKeyRef>>();
                foreach (var grass in ltex.Grasses)
                {
                    var grassModNameOffset = builder.CreateString(grass.FormKey.ModKey.FileName);
                    ASMutagenBuffers.FormKeyRef.StartFormKeyRef(builder);
                    ASMutagenBuffers.FormKeyRef.AddModName(builder, grassModNameOffset);
                    ASMutagenBuffers.FormKeyRef.AddFormId(builder, grass.FormKey.ID);
                    grassOffsets.Add(ASMutagenBuffers.FormKeyRef.EndFormKeyRef(builder));
                }
                var grassesVec = ASMutagenBuffers.LandscapeTextureEntry.CreateGrassesVector(builder, [.. grassOffsets]);

                ASMutagenBuffers.LandscapeTextureEntry.StartLandscapeTextureEntry(builder);
                ASMutagenBuffers.LandscapeTextureEntry.AddModName(builder, modNameOffset);
                ASMutagenBuffers.LandscapeTextureEntry.AddFormId(builder, ltex.FormKey.ID);
                ASMutagenBuffers.LandscapeTextureEntry.AddSlots(builder, slotsOffset);
                ASMutagenBuffers.LandscapeTextureEntry.AddEditorId(builder, editorIdOffset);
                ASMutagenBuffers.LandscapeTextureEntry.AddGrasses(builder, grassesVec);
                entryOffsets.Add(ASMutagenBuffers.LandscapeTextureEntry.EndLandscapeTextureEntry(builder));
            }

            var entriesVector = ASMutagenBuffers.LandscapeTextureEntries.CreateEntriesVector(builder, [.. entryOffsets]);
            ASMutagenBuffers.LandscapeTextureEntries.StartLandscapeTextureEntries(builder);
            ASMutagenBuffers.LandscapeTextureEntries.AddEntries(builder, entriesVector);
            var rootOffset = ASMutagenBuffers.LandscapeTextureEntries.EndLandscapeTextureEntries(builder);
            builder.Finish(rootOffset.Value);

            var byteArray = builder.SizedByteArray();
            *outLength = (uint)byteArray.Length;
            *outBufferPtr = (byte*)Marshal.AllocHGlobal(byteArray.Length);
            Marshal.Copy(byteArray, 0, (IntPtr)(*outBufferPtr), byteArray.Length);
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    // Looks up a GRAS record's own mesh path, for the C++ side to check for a seasonal sibling
    // NIF on disk (a different mesh file entirely, not an AlternateTextures override - grass is
    // never placed via a REFR, so there's no per-instance texture-override slot to use here).
    [UnmanagedCallersOnly(EntryPoint = "GetGrassMeshPath", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void GetGrassMeshPath(
      [DNNE.C99Type("const wchar_t*")] IntPtr modNamePtr,
      uint formId,
      [DNNE.C99Type("wchar_t**")] IntPtr* outMeshPathPtr)
    {
        try
        {
            if (Env is null)
            {
                throw new Exception("Initialize must be called before GetGrassMeshPath");
            }

            if (outMeshPathPtr is null)
            {
                throw new Exception("outMeshPathPtr is null");
            }

            string modName = Marshal.PtrToStringUni(modNamePtr) ?? string.Empty;
            var formKey = new FormKey(ModKey.FromFileName(modName), formId);

            if (!Env.LinkCache.TryResolve<IGrassGetter>(formKey, out var grass) || grass.Model is null || grass.Model.File.IsNull)
            {
                return;
            }

            *outMeshPathPtr = Marshal.StringToHGlobalUni(AddPrefixIfNotExists("meshes\\", grass.Model.File.GivenPath));
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    // Duplicates a GRAS record with its Model.File pointed at a different (season-suffixed) mesh
    // found on disk - unlike the STAT/ACTI/etc. duplication path, this never touches
    // AlternateTextures/TXST at all, since grass appearance comes entirely from whichever mesh
    // the GRAS record's own Model references.
    [UnmanagedCallersOnly(EntryPoint = "DuplicateGrass", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void DuplicateGrass(
      [DNNE.C99Type("const wchar_t*")] IntPtr modNamePtr,
      uint formId,
      [DNNE.C99Type("const wchar_t*")] IntPtr newMeshPathPtr,
      [DNNE.C99Type("const wchar_t*")] IntPtr seasonSuffixPtr,
      [DNNE.C99Type("wchar_t**")] IntPtr* outNewModNamePtr,
      [DNNE.C99Type("unsigned int*")] uint* outNewFormId)
    {
        try
        {
            if (Env is null || SeasonsMod is null)
            {
                throw new Exception("Initialize must be called before DuplicateGrass");
            }

            if (outNewModNamePtr is null || outNewFormId is null)
            {
                throw new Exception("output pointer is null");
            }

            string modName = Marshal.PtrToStringUni(modNamePtr) ?? string.Empty;
            string newMeshPath = Marshal.PtrToStringUni(newMeshPathPtr) ?? string.Empty;
            string seasonSuffix = Marshal.PtrToStringUni(seasonSuffixPtr) ?? string.Empty;
            var formKey = new FormKey(ModKey.FromFileName(modName), formId);

            if (!Env.LinkCache.TryResolve<IGrassGetter>(formKey, out var existingGrass))
            {
                return;
            }

            var newRecordFormKey = new FormKey(SeasonsMod.ModKey, GetLowestAvailableFormID());
            var duplicated = existingGrass.Duplicate(newRecordFormKey);

            if (duplicated is not Grass newGrass || newGrass.Model is null)
            {
                MessageHandler.Log("DuplicateGrass: duplicated record has no Model for " + GetRecordDesc(existingGrass), 4);
                return;
            }

            var baseEDID = existingGrass.EditorID.IsNullOrEmpty() ? ("Grass" + newRecordFormKey.ID.ToString("X6")) : existingGrass.EditorID;
            newGrass.EditorID = baseEDID + "_" + seasonSuffix;
            newGrass.Model.File.TrySetPath(RemovePrefixIfExists("meshes\\", newMeshPath));

            SeasonsMod.Grasses.Add(newGrass);

            *outNewFormId = newRecordFormKey.ID;
            *outNewModNamePtr = Marshal.StringToHGlobalUni(SeasonsMod.ModKey.FileName);
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    // Looks up the winning "LandscapeDefault" TextureSet - the record TerrainHelper.esp defines
    // so mod authors can override the engine's default (no-LTEX-assigned) terrain texture with
    // more than just diffuse/normal (normally only settable via skyrim.ini). Returns its current
    // diffuse texture path, or nothing if TerrainHelper.esp isn't in the load order (i.e. no
    // "LandscapeDefault" record exists anywhere to find).
    [UnmanagedCallersOnly(EntryPoint = "GetLandscapeDefaultDiffuse", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void GetLandscapeDefaultDiffuse([DNNE.C99Type("wchar_t**")] IntPtr* outDiffusePtr)
    {
        try
        {
            if (Env is null)
            {
                throw new Exception("Initialize must be called before GetLandscapeDefaultDiffuse");
            }

            if (outDiffusePtr is null)
            {
                throw new Exception("outDiffusePtr is null");
            }

            var landscapeDefault = Env.LoadOrder.PriorityOrder.TextureSet().WinningOverrides()
                .FirstOrDefault(t => t.EditorID == "LandscapeDefault");
            if (landscapeDefault is null || landscapeDefault.Diffuse.IsNullOrEmpty())
            {
                return;
            }

            // TXST fields are stored relative to textures\ (e.g. "Landscape\Dirt02.dds") - add the
            // prefix back so this matches the "textures\..." convention used everywhere on the C++
            // side (PGDirectory's own texture path set), same as EnumerateLandscapeTextures does.
            var diffuseWithPrefix = AddPrefixIfNotExists("textures\\", landscapeDefault.Diffuse);
            *outDiffusePtr = Marshal.StringToHGlobalUni(diffuseWithPrefix);
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    // Overrides TerrainHelper.esp's "LandscapeDefault" TextureSet with a Height (parallax) slot
    // pointing at parallaxPath - this is what switches on TerrainHelper's extended terrain shader
    // globally, letting it read the parallax slot off every other landscape texture set in the
    // game too. Not season-specific - this is the engine's single, always-active fallback
    // texture, not a per-season swappable record. A no-op if TerrainHelper.esp isn't in the load
    // order (no "LandscapeDefault" record to find). Mutagen automatically adds TerrainHelper.esp
    // as a master of SeasonsMod at write time, since the override reuses its FormKey.
    [UnmanagedCallersOnly(EntryPoint = "PatchLandscapeDefaultParallax", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void PatchLandscapeDefaultParallax([DNNE.C99Type("const wchar_t*")] IntPtr parallaxPathPtr)
    {
        try
        {
            if (Env is null || SeasonsMod is null)
            {
                throw new Exception("Initialize must be called before PatchLandscapeDefaultParallax");
            }

            string parallaxPath = Marshal.PtrToStringUni(parallaxPathPtr) ?? string.Empty;
            if (parallaxPath.IsNullOrEmpty())
            {
                return;
            }

            var landscapeDefault = Env.LoadOrder.PriorityOrder.TextureSet().WinningOverrides()
                .FirstOrDefault(t => t.EditorID == "LandscapeDefault");
            if (landscapeDefault is null)
            {
                return;
            }

            var overrideTXST = SeasonsMod.TextureSets.GetOrAddAsOverride(landscapeDefault);
            overrideTXST.Height = RemovePrefixIfExists("textures\\", parallaxPath);
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    // Duplicates existing model-referencing records (currently STAT only) into new records
    // pointing at a seasonal texture set, leaving the originals untouched. Used to build
    // the base/swap FormID pairs consumed by Seasons of Skyrim's Data/Seasons/*.ini files.
    [UnmanagedCallersOnly(EntryPoint = "GenerateSeasonalDuplicates", CallConvs = [typeof(CallConvCdecl)])]
    public static unsafe void GenerateSeasonalDuplicates(
      [DNNE.C99Type("const unsigned int")] uint length,
      [DNNE.C99Type("const uint8_t*")] byte* bufferPtr,
      [DNNE.C99Type("unsigned int*")] uint* outLength,
      [DNNE.C99Type("uint8_t**")] byte** outBufferPtr)
    {
        try
        {
            if (Env is null)
            {
                throw new Exception("Initialize must be called before GenerateSeasonalDuplicates");
            }

            if (outLength is null)
            {
                throw new Exception("outLength pointer is null");
            }

            if (length == 0 || bufferPtr == null)
            {
                *outLength = 0;
                return;
            }

            Span<byte> bufferSpan = new(bufferPtr, (int)length);
            var buffer = new ByteBuffer(bufferSpan.ToArray());
            var requests = ASMutagenBuffers.SeasonalDuplicateRequests.GetRootAsSeasonalDuplicateRequests(buffer);

            var resultBuilder = new FlatBufferBuilder(1024);
            var resultOffsets = new List<Offset<ASMutagenBuffers.SeasonalDuplicateResult>>();

            for (int i = 0; i < requests.RequestsLength; i++)
            {
                var reqContainer = requests.Requests(i);
                if (!reqContainer.HasValue)
                {
                    continue;
                }

                var req = reqContainer.Value;
                uint newFormId = 0;
                string newModName = string.Empty;

                var searchFormKey = new FormKey(req.ModName, req.FormId);

                // Resolves against the current winning override across the whole load order -
                // naturally picks up CM/PBR/parallax textures if the user has PGPatcher's own
                // output installed as a regular mod, no special-casing needed.
                Env.LinkCache.TryResolve<IMajorRecordGetter>(searchFormKey, out var existingRecord);

                if (existingRecord is not null)
                {
                    try
                    {
                        if (SeasonsMod is null)
                        {
                            throw new Exception("SeasonsMod is null in GenerateSeasonalDuplicates");
                        }

                        // Find or create the texture set record for one shape's override.
                        ITextureSet GetOrCreateSeasonalTXST(string[] slots)
                        {
                            if (NewTextureSets.TryGetValue(slots, out ITextureSet? existingTXST))
                            {
                                return existingTXST;
                            }

                            var txstFormKey = new FormKey(SeasonsMod.ModKey, GetLowestAvailableFormID());
                            var diffuseTex = slots[0].IsNullOrEmpty() ? "" : Path.GetFileNameWithoutExtension(slots[0]);
                            // Suffix LTEX-sourced texture sets so they're distinguishable from STAT-sourced ones in xEdit.
                            var newTXSTEDID = diffuseTex.IsNullOrEmpty()
                                ? ("TXST" + txstFormKey.ID.ToString("X6"))
                                : (req.Type == "LTEX" ? diffuseTex + "_ltex" : diffuseTex);

                            var createdTXST = new TextureSet(txstFormKey, Env.GameRelease.ToSkyrimRelease())
                            {
                                Diffuse = slots[0].IsNullOrEmpty() ? null : RemovePrefixIfExists("textures\\", slots[0]),
                                NormalOrGloss = slots[1].IsNullOrEmpty() ? null : RemovePrefixIfExists("textures\\", slots[1]),
                                GlowOrDetailMap = slots[2].IsNullOrEmpty() ? null : RemovePrefixIfExists("textures\\", slots[2]),
                                Height = slots[3].IsNullOrEmpty() ? null : RemovePrefixIfExists("textures\\", slots[3]),
                                Environment = slots[4].IsNullOrEmpty() ? null : RemovePrefixIfExists("textures\\", slots[4]),
                                EnvironmentMaskOrSubsurfaceTint = slots[5].IsNullOrEmpty() ? null : RemovePrefixIfExists("textures\\", slots[5]),
                                Multilayer = slots[6].IsNullOrEmpty() ? null : RemovePrefixIfExists("textures\\", slots[6]),
                                BacklightMaskOrSpecular = slots[7].IsNullOrEmpty() ? null : RemovePrefixIfExists("textures\\", slots[7]),
                                EditorID = newTXSTEDID
                            };

                            SeasonsMod.TextureSets.Add(createdTXST);
                            allocatedFormIDs.Add(txstFormKey.ID);
                            lastUsedFormID = txstFormKey.ID;
                            NewTextureSets[slots] = createdTXST;
                            return createdTXST;
                        }

                        // Applies this request's per-shape AlternateTextures overrides to any
                        // placed-object record's Model - shared across Static/Activator/Furniture/
                        // MoveableStatic, which all expose the same nullable Model.AlternateTextures shape.
                        void ApplyAlternateTextureOverrides(Model? model)
                        {
                            if (model is null)
                            {
                                return;
                            }

                            // May be null (not just empty) for records that never had any
                            // alternate texture - FirstOrDefault/Add below both need a real list.
                            model.AlternateTextures ??= [];

                            for (int o = 0; o < req.OverridesLength; o++)
                            {
                                var overrideContainer = req.Overrides(o);
                                if (!overrideContainer.HasValue)
                                {
                                    continue;
                                }

                                var overrideVal = overrideContainer.Value;
                                if (!overrideVal.Slots.HasValue || overrideVal.Slots.Value.TexturesLength != 8)
                                {
                                    continue;
                                }

                                string[] slots = new string[8];
                                for (int k = 0; k < 8; k++)
                                {
                                    slots[k] = overrideVal.Slots.Value.Textures(k) ?? string.Empty;
                                }

                                var newTXSTObj = GetOrCreateSeasonalTXST(slots);

                                // Match by the shape's real plugin Index - never assume position 0.
                                var existingEntry = model.AlternateTextures
                                    .FirstOrDefault(a => a.Index == overrideVal.ShapeIndex);
                                if (existingEntry is not null)
                                {
                                    existingEntry.NewTexture.FormKey = newTXSTObj.FormKey;
                                }
                                else
                                {
                                    // No pre-existing override for this shape: author a new entry
                                    // using the shape's real NIF name/index, not a placeholder.
                                    model.AlternateTextures.Add(new AlternateTexture()
                                    {
                                        Name = overrideVal.ShapeName ?? string.Empty,
                                        Index = overrideVal.ShapeIndex,
                                        NewTexture = new FormLinkNullable<ITextureSetGetter>(newTXSTObj.FormKey)
                                    });
                                }
                            }
                        }

                        // Duplicate the base record under a brand new FormID.
                        // Mark the ID allocated immediately - GetOrCreateSeasonalTXST below may call
                        // GetLowestAvailableFormID() again (once per shape override) and must not be
                        // handed this same still-unreserved ID.
                        var newRecordFormKey = new FormKey(SeasonsMod.ModKey, GetLowestAvailableFormID());
                        allocatedFormIDs.Add(newRecordFormKey.ID);
                        lastUsedFormID = newRecordFormKey.ID;
                        var duplicated = existingRecord.Duplicate(newRecordFormKey);

                        if (duplicated is SkyrimMajorRecord skyrimRec)
                        {
                            var baseEDID = existingRecord.EditorID.IsNullOrEmpty() ? ("Form" + newRecordFormKey.ID.ToString("X6")) : existingRecord.EditorID;
                            skyrimRec.EditorID = baseEDID + "_" + req.SeasonSuffix;
                        }

                        if (duplicated is Static newStat)
                        {
                            ApplyAlternateTextureOverrides(newStat.Model);
                            SeasonsMod.Statics.Add(newStat);

                            newFormId = newRecordFormKey.ID;
                            newModName = SeasonsMod.ModKey.FileName;
                        }
                        else if (duplicated is Mutagen.Bethesda.Skyrim.Activator newActivator)
                        {
                            ApplyAlternateTextureOverrides(newActivator.Model);
                            SeasonsMod.Activators.Add(newActivator);

                            newFormId = newRecordFormKey.ID;
                            newModName = SeasonsMod.ModKey.FileName;
                        }
                        else if (duplicated is Furniture newFurniture)
                        {
                            ApplyAlternateTextureOverrides(newFurniture.Model);
                            SeasonsMod.Furniture.Add(newFurniture);

                            newFormId = newRecordFormKey.ID;
                            newModName = SeasonsMod.ModKey.FileName;
                        }
                        else if (duplicated is MoveableStatic newMoveableStatic)
                        {
                            ApplyAlternateTextureOverrides(newMoveableStatic.Model);
                            SeasonsMod.MoveableStatics.Add(newMoveableStatic);

                            newFormId = newRecordFormKey.ID;
                            newModName = SeasonsMod.ModKey.FileName;
                        }
                        else if (duplicated is Tree newTree)
                        {
                            ApplyAlternateTextureOverrides(newTree.Model);
                            SeasonsMod.Trees.Add(newTree);

                            newFormId = newRecordFormKey.ID;
                            newModName = SeasonsMod.ModKey.FileName;
                        }
                        else if (duplicated is Flora newFlora)
                        {
                            ApplyAlternateTextureOverrides(newFlora.Model);
                            SeasonsMod.Florae.Add(newFlora);

                            newFormId = newRecordFormKey.ID;
                            newModName = SeasonsMod.ModKey.FileName;
                        }
                        else if (duplicated is LandscapeTexture newLtex)
                        {
                            // LTEX has no shape/AlternateTextures concept - always exactly one override.
                            var overrideContainer = req.OverridesLength > 0 ? req.Overrides(0) : null;
                            if (overrideContainer.HasValue && overrideContainer.Value.Slots.HasValue
                                && overrideContainer.Value.Slots.Value.TexturesLength == 8)
                            {
                                var overrideVal = overrideContainer.Value;
                                string[] slots = new string[8];
                                for (int k = 0; k < 8; k++)
                                {
                                    slots[k] = overrideVal.Slots.Value.Textures(k) ?? string.Empty;
                                }

                                var newTXSTObj = GetOrCreateSeasonalTXST(slots);
                                newLtex.TextureSet.SetTo(newTXSTObj.FormKey);
                            }

                            if (req.OverrideGrasses)
                            {
                                newLtex.Grasses.Clear();
                                for (int g = 0; g < req.GrassOverridesLength; g++)
                                {
                                    var grassRef = req.GrassOverrides(g);
                                    if (grassRef.HasValue && !grassRef.Value.ModName.IsNullOrEmpty())
                                    {
                                        var grassFormKey = new FormKey(ModKey.FromFileName(grassRef.Value.ModName!), grassRef.Value.FormId);
                                        newLtex.Grasses.Add(new FormLink<IGrassGetter>(grassFormKey));
                                    }
                                }
                            }

                            SeasonsMod.LandscapeTextures.Add(newLtex);

                            newFormId = newRecordFormKey.ID;
                            newModName = SeasonsMod.ModKey.FileName;
                        }
                        else
                        {
                            MessageHandler.Log("GenerateSeasonalDuplicates: unsupported record type for " + GetRecordDesc(existingRecord), 4);
                        }
                    }
                    catch (Exception ex)
                    {
                        MessageHandler.Log("Failed to generate seasonal duplicate for " + GetRecordDesc(existingRecord) + ": " + ex.Message, 4);
                    }
                }

                var modNameOffset = resultBuilder.CreateString(req.ModName);
                var newModNameOffset = resultBuilder.CreateString(newModName);

                ASMutagenBuffers.SeasonalDuplicateResult.StartSeasonalDuplicateResult(resultBuilder);
                ASMutagenBuffers.SeasonalDuplicateResult.AddModName(resultBuilder, modNameOffset);
                ASMutagenBuffers.SeasonalDuplicateResult.AddFormId(resultBuilder, req.FormId);
                ASMutagenBuffers.SeasonalDuplicateResult.AddNewModName(resultBuilder, newModNameOffset);
                ASMutagenBuffers.SeasonalDuplicateResult.AddNewFormId(resultBuilder, newFormId);
                var resultOffset = ASMutagenBuffers.SeasonalDuplicateResult.EndSeasonalDuplicateResult(resultBuilder);
                resultOffsets.Add(resultOffset);
            }

            var resultsVector = ASMutagenBuffers.SeasonalDuplicateResults.CreateResultsVector(resultBuilder, [.. resultOffsets]);
            ASMutagenBuffers.SeasonalDuplicateResults.StartSeasonalDuplicateResults(resultBuilder);
            ASMutagenBuffers.SeasonalDuplicateResults.AddResults(resultBuilder, resultsVector);
            var rootOffset = ASMutagenBuffers.SeasonalDuplicateResults.EndSeasonalDuplicateResults(resultBuilder);
            resultBuilder.Finish(rootOffset.Value);

            var byteArray = resultBuilder.SizedByteArray();
            *outLength = (uint)byteArray.Length;
            *outBufferPtr = (byte*)Marshal.AllocHGlobal(byteArray.Length);
            Marshal.Copy(byteArray, 0, (IntPtr)(*outBufferPtr), byteArray.Length);
        }
        catch (Exception ex)
        {
            ExceptionHandler.SetLastException(ex);
        }
    }

    //
    // Helpers
    //

    private static List<Tuple<IModelGetter, string>> GetModelElems(IMajorRecordGetter Rec)
    {
        // Will store models to check later
        List<Tuple<IModelGetter, string>> ModelRecs = [];

        // Figure out special cases for nested models
        if (Rec is IArmorGetter armorObj && armorObj.WorldModel is not null)
        {
            if (armorObj.WorldModel.Male is not null && armorObj.WorldModel.Male.Model is not null)
            {
                ModelRecs.Add(new Tuple<IModelGetter, string>(armorObj.WorldModel.Male.Model, "MALE"));
            }

            if (armorObj.WorldModel.Female is not null && armorObj.WorldModel.Female.Model is not null)
            {
                ModelRecs.Add(new Tuple<IModelGetter, string>(armorObj.WorldModel.Female.Model, "FEMALE"));
            }

            if (armorObj.Destructible is not null && armorObj.Destructible.Stages is not null)
            {
                AddDestructibleStages(armorObj.Destructible.Stages, ModelRecs);
            }

            return ModelRecs;
        }
        else if (Rec is IArmorAddonGetter armorAddonObj)
        {
            if (armorAddonObj.WorldModel is not null)
            {
                if (armorAddonObj.WorldModel.Male is not null)
                {
                    ModelRecs.Add(new Tuple<IModelGetter, string>(armorAddonObj.WorldModel.Male, "MALE"));
                }

                if (armorAddonObj.WorldModel.Female is not null)
                {
                    ModelRecs.Add(new Tuple<IModelGetter, string>(armorAddonObj.WorldModel.Female, "FEMALE"));
                }
            }

            if (armorAddonObj.FirstPersonModel is not null)
            {
                if (armorAddonObj.FirstPersonModel.Male is not null)
                {
                    ModelRecs.Add(new Tuple<IModelGetter, string>(armorAddonObj.FirstPersonModel.Male, "1STMALE"));
                }

                if (armorAddonObj.FirstPersonModel.Female is not null)
                {
                    ModelRecs.Add(new Tuple<IModelGetter, string>(armorAddonObj.FirstPersonModel.Female, "1STFEMALE"));
                }
            }

            return ModelRecs;
        }

        if (Rec is IModeledGetter modeledObj && modeledObj.Model is not null)
        {
            ModelRecs.Add(new Tuple<IModelGetter, string>(modeledObj.Model, "MODL"));
        }

        // Deal with destructibles
        if (Rec is IAmmunitionGetter ammoObj && ammoObj.Destructible is not null && ammoObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(ammoObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IActivatorGetter activatorObj && activatorObj.Destructible is not null && activatorObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(activatorObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IBookGetter bookObj && bookObj.Destructible is not null && bookObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(bookObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IContainerGetter containerObj && containerObj.Destructible is not null && containerObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(containerObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IDoorGetter doorObj && doorObj.Destructible is not null && doorObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(doorObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IFloraGetter floraObj && floraObj.Destructible is not null && floraObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(floraObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IFurnitureGetter furnitureObj && furnitureObj.Destructible is not null && furnitureObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(furnitureObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IIngestibleGetter ingestibleObj && ingestibleObj.Destructible is not null && ingestibleObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(ingestibleObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IIngredientGetter ingredientObj && ingredientObj.Destructible is not null && ingredientObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(ingredientObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IKeyGetter keyObj && keyObj.Destructible is not null && keyObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(keyObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is ILightGetter lightObj && lightObj.Destructible is not null && lightObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(lightObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IMiscItemGetter miscObj && miscObj.Destructible is not null && miscObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(miscObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IMoveableStaticGetter moveableStaticObj && moveableStaticObj.Destructible is not null && moveableStaticObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(moveableStaticObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IProjectileGetter projectileObj && projectileObj.Destructible is not null && projectileObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(projectileObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IScrollGetter scrollObj && scrollObj.Destructible is not null && scrollObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(scrollObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is ISoulGemGetter soulGemObj && soulGemObj.Destructible is not null && soulGemObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(soulGemObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is ITalkingActivatorGetter talkingActivatorObj && talkingActivatorObj.Destructible is not null && talkingActivatorObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(talkingActivatorObj.Destructible.Stages, ModelRecs);
        }
        else if (Rec is IWeaponGetter weaponObj && weaponObj.Destructible is not null && weaponObj.Destructible.Stages is not null)
        {
            AddDestructibleStages(weaponObj.Destructible.Stages, ModelRecs);
        }

        return ModelRecs;
    }

    private static void AddDestructibleStages(IReadOnlyList<IDestructionStageGetter> stages, List<Tuple<IModelGetter, string>> ModelRecs)
    {
        for (int i = 0; i < stages.Count; i++)
        {
            var stage = stages[i];
            if (stage.Model is not null)
            {
                ModelRecs.Add(new Tuple<IModelGetter, string>(stage.Model, $"DMDL{i}"));
            }
        }
    }

    private static IModelGetter? GetModelElemBySubModel(IMajorRecordGetter Rec, string SubModel)
    {
        var ModelRecs = GetModelElems(Rec);

        foreach (var modelRec in ModelRecs)
        {
            if (modelRec.Item2 == SubModel)
            {
                return modelRec.Item1;
            }
        }

        return null;
    }

    private static IEnumerable<IMajorRecordGetter> EnumerateModelRecords()
    {
        if (Env is null)
        {
            return [];
        }

        return Env.LoadOrder.PriorityOrder.Activator().WinningOverrides()
                .Concat<IMajorRecordGetter>(Env.LoadOrder.PriorityOrder.AddonNode().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Ammunition().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.AnimatedObject().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Armor().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.ArmorAddon().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.ArtObject().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.BodyPartData().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Book().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.CameraShot().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Climate().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Container().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Door().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Explosion().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Flora().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Furniture().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Grass().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Hazard().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.HeadPart().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.IdleMarker().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Impact().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Ingestible().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Ingredient().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Key().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.LeveledNpc().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Light().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.MaterialObject().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.MiscItem().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.MoveableStatic().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Projectile().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Scroll().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.SoulGem().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Static().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.TalkingActivator().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Tree().WinningOverrides())
                .Concat(Env.LoadOrder.PriorityOrder.Weapon().WinningOverrides());
    }

    private static IEnumerable<IMajorRecordGetter> EnumerateModelRecordsSafe()
    {
        using (var enumerator = EnumerateModelRecords().GetEnumerator())
        {
            bool next = true;

            while (next)
            {
                try
                {
                    next = enumerator.MoveNext();
                }
                catch (RecordException ex)
                {
                    var innerEx = ex.InnerException;
                    if (innerEx is null)
                    {
                        MessageHandler.Log("Unknown error with mod " + ex.ModKey, 3);
                    }
                    else
                    {
                        MessageHandler.Log(innerEx.ToString(), 3);
                    }

                    continue;
                }

                if (next)
                    yield return enumerator.Current;
            }
        }
    }

    private static string[] GetTextureSet(ITextureSetGetter textureSet)
    {
        try
        {
            return
            [
                textureSet.Diffuse?.ToString().ToLowerInvariant() ?? string.Empty,
                textureSet.NormalOrGloss?.ToString().ToLowerInvariant() ?? string.Empty,
                textureSet.GlowOrDetailMap?.ToString().ToLowerInvariant() ?? string.Empty,
                textureSet.Height?.ToString().ToLowerInvariant() ?? string.Empty,
                textureSet.Environment?.ToString().ToLowerInvariant() ?? string.Empty,
                textureSet.EnvironmentMaskOrSubsurfaceTint?.ToString().ToLowerInvariant() ?? string.Empty,
                textureSet.Multilayer?.ToString().ToLowerInvariant() ?? string.Empty,
                textureSet.BacklightMaskOrSpecular?.ToString().ToLowerInvariant() ?? string.Empty
            ];
        }
        catch (Exception)
        {
            MessageHandler.Log("Unable to read texture set: " + GetRecordDesc(textureSet), 3);
            return [.. Enumerable.Repeat(string.Empty, 8)];
        }
    }

    private static string GetRecordDesc(IMajorRecordGetter rec)
    {
        return rec.FormKey.ModKey.FileName + " / " + rec.FormKey.ID.ToString("X6");
    }

    private static string GetRecordDesc(FormKey rec)
    {
        return rec.ModKey.FileName + " / " + rec.ID.ToString("X6");
    }

    private static string RemovePrefixIfExists(string prefix, string str)
    {
        if (str.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            return str[prefix.Length..];
        }
        return str;
    }

    private static string AddPrefixIfNotExists(string prefix, string str)
    {
        if (!str.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            return prefix + str;
        }
        return str;
    }

    private static uint GetLowestAvailableFormID()
    {
        for (uint id = lastUsedFormID + 1; id <= 0xFFFFFF; id++)
        {
            if (!allocatedFormIDs.Contains(id))
            {
                return id;
            }
        }

        throw new Exception("No available FormIDs left in plugin");
    }

    private static string GetXEditTypeFromType(IMajorRecordGetter rec)
    {
        if (rec is IActivatorGetter) return "ACTI";
        if (rec is IAmmunitionGetter) return "AMMO";
        if (rec is IAnimatedObjectGetter) return "ANIO";
        if (rec is IArmorGetter) return "ARMO";
        if (rec is IArmorAddonGetter) return "ARMA";
        if (rec is IArtObjectGetter) return "ARTO";
        if (rec is IBodyPartDataGetter) return "BPTD";
        if (rec is IBookGetter) return "BOOK";
        if (rec is ICameraShotGetter) return "CAMS";
        if (rec is IClimateGetter) return "CLMT";
        if (rec is IContainerGetter) return "CONT";
        if (rec is IDoorGetter) return "DOOR";
        if (rec is IExplosionGetter) return "EXPL";
        if (rec is IFloraGetter) return "FLOR";
        if (rec is IFurnitureGetter) return "FURN";
        if (rec is IGrassGetter) return "GRAS";
        if (rec is IHazardGetter) return "HAZD";
        if (rec is IHeadPartGetter) return "HDPT";
        if (rec is IIdleMarkerGetter) return "IDLM";
        if (rec is IImpactGetter) return "IPCT";
        if (rec is IIngestibleGetter) return "ALCH";
        if (rec is IIngredientGetter) return "INGR";
        if (rec is IKeyGetter) return "KEYM";
        if (rec is ILeveledNpcGetter) return "LVLN";
        if (rec is ILightGetter) return "LIGH";
        if (rec is IMaterialObjectGetter) return "MATO";
        if (rec is IMiscItemGetter) return "MISC";
        if (rec is IMoveableStaticGetter) return "MSTT";
        if (rec is IProjectileGetter) return "PROJ";
        if (rec is IScrollGetter) return "SCRL";
        if (rec is ISoulGemGetter) return "SLGM";
        if (rec is IStaticGetter) return "STAT";
        if (rec is ITalkingActivatorGetter) return "TACT";
        if (rec is ITreeGetter) return "TREE";
        if (rec is IWeaponGetter) return "WEAP";

        return "";
    }
}
