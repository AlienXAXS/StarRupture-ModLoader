# RailJunctionFixer

This plugin fixes mass entity rail junction issues in the game by modifying the UE5 reflection system to change the inheritance hierarchy of `FCrLogisticsSocketsFragment`.

## What it does

The plugin dynamically patches the game's type system at runtime to make `FCrLogisticsSocketsFragment` inherit from `FCrMassSavableFragment` instead of the default `FMassFragment`. This ensures that logistics data is properly saved/loaded with mass entities, fixing issues with rail junctions not being properly saved.

## How it works

At initialization, the plugin:

1. Uses UE5's reflection system to find the `UScriptStruct` instances for both:
   - `FCrLogisticsSocketsFragment` (the fragment type that needs fixing)
   - `FCrMassSavableFragment` (the base class that provides save functionality)

2. Modifies the `SuperStruct` pointer of `FCrLogisticsSocketsFragment` to point to `FCrMassSavableFragment` instead of `FMassFragment`

3. This changes the inheritance hierarchy at runtime:
   - **Before**: `FCrLogisticsSocketsFragment` ? `FMassFragment`
   - **After**: `FCrLogisticsSocketsFragment` ? `FCrMassSavableFragment` ? `FMassFragment`

4. The change ensures that logistics data is included in the mass entity save system

## Technical Details

The implementation uses:
- UE5 SDK's reflection system (`UObject::FindObject<UScriptStruct>()`)
- Direct memory patching of the `SuperStruct` field (offset `0x40` in `UStruct`)
- Proper memory protection handling (`VirtualProtect`)

## Building

This plugin is part of the StarRupture-ModLoader workspace. Build the entire solution to compile all plugins including this one.

## Logging

The plugin logs detailed information about:
- Finding the struct types
- Current and new parent class information
- Success/failure of the patching operation

Check the mod loader logs for plugin initialization status.
