﻿#include "DetourManager.h"
#include "PaliaOverlay.h"
#include <SDK/Palia_parameters.hpp>

#include "SDKExt.h"
#include "Utils.h"
#include "format"
#include "functional"

using namespace SDK;

// Cache

void ClearActorCache(PaliaOverlay* Overlay) {
    const UWorld* World = GetWorld();

    const auto GameplayStatics = static_cast<UGameplayStatics*>(UGameplayStatics::StaticClass()->DefaultObject);
    if (!GameplayStatics)
        return;

    // Clear cache on level change
    if (Overlay->CurrentLevel != World->PersistentLevel) {
        Overlay->CachedActors.clear();
        Overlay->CurrentLevel = World->PersistentLevel;
        Overlay->CurrentMap = GameplayStatics->GetCurrentLevelName(World, false).ToString();
    }
}

void ManageActorCache(PaliaOverlay* Overlay) {
    const UWorld* World = GetWorld();

    const auto GameplayStatics = static_cast<UGameplayStatics*>(UGameplayStatics::StaticClass()->DefaultObject);
    if (!GameplayStatics)
        return;

    if (const double WorldTime = GameplayStatics->GetTimeSeconds(World); abs(WorldTime - Overlay->LastCachedTime) > 0.1) {
        Overlay->LastCachedTime = WorldTime;
        Overlay->ProcessActors(Overlay->ActorStep);

        Overlay->ActorStep++;
        if (Overlay->ActorStep >= static_cast<int>(EType::MAX)) {
            Overlay->ActorStep = 0;
        }
    }
}

// [Fun]

inline void Func_DoTeleportToTargeted(PaliaOverlay* Overlay, const double BestScore) {
    if (Overlay->bTeleportToTargeted) {
        const auto now = std::chrono::steady_clock::now();
        if (IsKeyHeld(VK_XBUTTON2) && std::abs(BestScore - FLT_MAX) > 0.0001f) {
            if (duration_cast<std::chrono::seconds>(now - Overlay->LastTeleportToTargetTime).count() >= 2) {
                const auto ValeriaCharacter = GetValeriaCharacter();

                if (!ValeriaCharacter)
                    return;

                bool shouldTeleport = true;

                // Avoid teleporting to players
                if (Overlay->bAvoidTeleportingToPlayers && Overlay->BestTargetActorType == EType::Players) {
                    shouldTeleport = false;
                }

                // Avoid teleporting to targeted if there are nerby players
                if (Overlay->bDoRadiusPlayersAvoidance && shouldTeleport) {
                    for (auto& [Actor, WorldPosition, DisplayName, ActorType, Type, Quality, Variant, shouldAdd] : Overlay->CachedActors) {
                        if (ActorType == EType::Players) {
                            if (!Actor || !Actor->IsValidLowLevel() || Actor->IsDefaultObject() || WorldPosition.IsZero())
                                continue;

                            // Don't count itself or us
                            if (Actor == Overlay->BestTargetActor || Actor == ValeriaCharacter)
                                continue;

                            // Check for actors within 30 meters of this actor
                            if (WorldPosition.GetDistanceToInMeters(Overlay->BestTargetActor->K2_GetActorLocation()) < 30) {
                                shouldTeleport = false;
                                break;
                            }
                        }
                    }
                }

                // Teleportation logic
                if (shouldTeleport) {
                    FVector TargetLocation = Overlay->BestTargetLocation;
                    TargetLocation.Z += 150.0f;

                    FHitResult HitResult;
                    ValeriaCharacter->K2_SetActorLocation(TargetLocation, false, &HitResult, true);
                    Overlay->LastTeleportToTargetTime = now;
                }
            }
        }
    }
}

inline void Func_DoTeleportToWaypoint(const PaliaOverlay* Overlay, const Params::TrackingComponent_RpcClient_SetUserMarkerViaWorldMap* SetUserMarkerViaWorldMap) {
    if (Overlay->bEnableWaypointTeleport) {
        const auto ValeriaCharacter = GetValeriaCharacter();
        if (ValeriaCharacter) {
            FVector TargetLocation = SetUserMarkerViaWorldMap->MarkerLocation;
            if (!TargetLocation.IsZero()) {
                TargetLocation.Z += 150.0f;
                ValeriaCharacter->K2_SetActorLocation(TargetLocation, false, nullptr, true);
            }
        }
    }
}

// [HUD]

inline void DrawCircle(UCanvas* Canvas, const float Radius, const int32 NumSegments, const FLinearColor Color, const float Thickness = 1.0f) {
    // Calculate screen center more accurately
    const FVector2D ScreenCenter = { static_cast<double>(Canvas->ClipX) / 2.0, static_cast<double>(Canvas->ClipY) / 2.0 };

    const double Increment = 360.0 / static_cast<double>(NumSegments);
    FVector2D LastPos = { ScreenCenter.X + Radius, ScreenCenter.Y };

    for (int i = 1; i <= NumSegments; i++) {
        const float Rad = CustomMath::DegreesToRadians(static_cast<float>(Increment * i));
        FVector2D NewPos = { ScreenCenter.X + Radius * cos(Rad), ScreenCenter.Y + Radius * sin(Rad) };
        Canvas->K2_DrawLine(LastPos, NewPos, Thickness, Color);
        LastPos = NewPos;
    }
}

inline void Func_DoInteliAim(PaliaOverlay* Overlay) {
    if (!Overlay->bEnableAimbot && !Overlay->bDrawFOVCircle)
        return;

    UWorld* World = GetWorld();
    const auto PlayerController = GetPlayerController();
    const auto ValeriaCharacter = GetValeriaCharacter();

    if (!PlayerController || !ValeriaCharacter)
        return;

    FVector CharacterLocation = ValeriaCharacter->K2_GetActorLocation();
    FRotator CharacterRotation = PlayerController->GetControlRotation();
    FVector ForwardVector = UKismetMathLibrary::GetForwardVector(CharacterRotation);
    double BestScore = FLT_MAX; // Using a scoring system based on various factors such as distance, area fov, prediction

    for (auto& [Actor, WorldPosition, DisplayName, ActorType, Type, Quality, Variant, shouldAdd] : Overlay->CachedActors) {
        if (!Actor || !Actor->IsValidLowLevel() || Actor->IsDefaultObject() || WorldPosition.IsZero())
            continue;

        bool bShouldConsider = false;

        switch (ActorType) {
        case EType::Animal:
            bShouldConsider = Overlay->Animals[Type][Variant]; // Toggle for different types of animals
            break;
        case EType::Ore:
            bShouldConsider = Overlay->Ores[Type][Variant]; // Toggle for different types of ores
            break;
        case EType::Bug:
            bShouldConsider = Overlay->Bugs[Type][Variant][Quality]; // Toggle for different types of bugs
            break;
        case EType::Forage:
            bShouldConsider = Overlay->Forageables[Type][Quality]; // Toggle for forageable items
            break;
        case EType::Players:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::Player)]; // Toggle for player visibility
            break;
        case EType::NPCs:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::NPC)]; // Toggle for NPCs
            break;
        case EType::Quest:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::Quest)]; // Toggle for quest items
            break;
        case EType::Loot:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::Loot)]; // Toggle for loot
            break;
        case EType::RummagePiles:
            if (Overlay->Singles[static_cast<int>(EOneOffs::RummagePiles)]) {
                if (Overlay->bVisualizeDefault) {
                    bShouldConsider = true;
                    break;
                }

                if (auto Pile = static_cast<ATimedLootPile*>(Actor)) {
                    if (ValeriaCharacter && Pile->CanGather(ValeriaCharacter) && Pile->bActivated) {
                        bShouldConsider = true;
                        break;
                    }
                }
            }
            break;
        case EType::Stables:
            bShouldConsider = Overlay->Singles[static_cast<int>(EOneOffs::Stables)]; // Toggle for Stables
            break;
        case EType::Tree:
            bShouldConsider = Overlay->Trees[Type][Variant]; // Toggle for trees
            break;
        case EType::Fish:
            bShouldConsider = Overlay->Fish[Type]; // Toggle for fish types
            break;
        default:
            break;
        }

        if (!bShouldConsider)
            continue;

        FVector ActorLocation = Actor->K2_GetActorLocation();
        FVector DirectionToActor = (ActorLocation - CharacterLocation).GetNormalized();
        FVector TargetVelocity = Actor->GetVelocity();

        FVector RelativeVelocity = TargetVelocity - ValeriaCharacter->GetVelocity();
        FVector RelativeDirection = RelativeVelocity.GetNormalized();

        double Distance = CharacterLocation.GetDistanceToInMeters(ActorLocation);
        float Angle = CustomMath::RadiansToDegrees(acosf(static_cast<float>(ForwardVector.Dot(DirectionToActor))));

        if (ActorLocation.IsZero())
            continue;

        if (Distance < 2.0)
            continue;
        if (Overlay->bEnableESPCulling && Distance > Overlay->CullDistance)
            continue;

        // Weighting factors for different factors
        double AngleWeight, DistanceWeight, MovementWeight;

        // Adjust weighting factors based on EType
        switch (ActorType) {
        case EType::Animal:
            AngleWeight = 0.10;
            DistanceWeight = 0.0;
            MovementWeight = 0.0;
            break;
        case EType::Ore:
            AngleWeight = 0.10;
            DistanceWeight = 0.0;
            MovementWeight = 0.0;
            break;
        case EType::Bug:
            AngleWeight = 0.10;
            DistanceWeight = 0.0;
            MovementWeight = 0.0;
            break;
        default:
            AngleWeight = 0.10;
            DistanceWeight = 0.0;
            MovementWeight = 0.0;
            break;
        }

        // Calculate score based on weighted sum of factors
        if (double Score = AngleWeight * Angle + DistanceWeight * Distance + MovementWeight * RelativeDirection.Magnitude(); Angle <= Overlay->FOVRadius / 2.0 && Score < Overlay->SelectionThreshold) {
            if (Score < BestScore) {
                BestScore = Score;
                Overlay->BestTargetActor = Actor;
                Overlay->BestTargetActorType = ActorType;
                Overlay->BestTargetLocation = ActorLocation;
                Overlay->BestTargetRotation = UKismetMathLibrary::FindLookAtRotation(CharacterLocation, ActorLocation);
            }
        }
    }

    Func_DoTeleportToTargeted(Overlay, BestScore);

    // Don't aimbot while the overlay is showing
    if (Overlay->bEnableAimbot && !Overlay->ShowOverlay()) {
        if (IsKeyHeld(VK_LBUTTON) && std::abs(BestScore - FLT_MAX) > 0.0001f) {
            // Only aimbot when a bow is equipped
            if (ValeriaCharacter->GetEquippedItem().ItemType->Name.ToString().find("Tool_Bow_") != std::string::npos) {
                bool IsAnimal = false;
                for (auto& [Actor, WorldPosition, DisplayName, ActorType, Type, Quality, Variant, shouldAdd] : Overlay->CachedActors) {
                    if (shouldAdd && ActorType == EType::Animal && Actor && Actor->IsValidLowLevel() && !Actor->IsDefaultObject()) {
                        if (FVector ActorLocation = Actor->K2_GetActorLocation(); ActorLocation == Overlay->BestTargetLocation) {
                            IsAnimal = true;
                            break;
                        }
                    }
                }
                // Adjust the aim rotation only if the selected best target is an animal
                if (IsAnimal) {
                    // Apply offset to pitch and yaw directly
                    FRotator TargetRotation = UKismetMathLibrary::FindLookAtRotation(CharacterLocation, Overlay->BestTargetLocation);
                    TargetRotation.Pitch += Overlay->AimOffset.X;
                    TargetRotation.Yaw += Overlay->AimOffset.Y;

                    // Smooth rotation adjustment
                    FRotator NewRotation = CustomMath::RInterpTo(CharacterRotation, TargetRotation, UGameplayStatics::GetTimeSeconds(World), Overlay->SmoothingFactor);
                    PlayerController->SetControlRotation(NewRotation);
                }
            }
        }
    }
}

inline void Func_DoESP(PaliaOverlay* Overlay, const AHUD* HUD) {
    if (!Overlay->bEnableESP) {
        Overlay->CachedActors.clear();
        return;
    }

    // Manage Cache Logic
    ClearActorCache(Overlay);
    ManageActorCache(Overlay);

    APlayerController* PlayerController = GetPlayerController();
    if (!PlayerController)
        return;

    FVector PawnLocation = PlayerController->K2_GetPawn()->K2_GetActorLocation();

    // Draw ESP Names Entities
    for (auto& [Actor, WorldPosition, DisplayName, ActorType, Type, Quality, Variant, shouldAdd] : Overlay->CachedActors) {
        FVector ActorLocation = WorldPosition;
        if (ActorType == EType::Animal || ActorType == EType::Bug || ActorType == EType::Players || ActorType == EType::Loot) {
            if (!Actor || !Actor->IsValidLowLevel() || Actor->IsDefaultObject())
                continue;

            if (ActorLocation = Actor->K2_GetActorLocation(); ActorLocation.IsZero())
                continue;
        }

        // Adjust Z coordinate for head-level display
        float HeightAdjustment = 100.0f; // Adjust this value based on typical actor height
        ActorLocation.Z += HeightAdjustment;

        double Distance = PawnLocation.GetDistanceToInMeters(ActorLocation);

        if (Distance < 2.0)
            continue;
        if (Overlay->bEnableESPCulling && Distance > Overlay->CullDistance)
            continue;

        FVector2D ScreenLocation;
        if (PlayerController->ProjectWorldLocationToScreen(ActorLocation, &ScreenLocation, true)) {
            ImU32 Color = IM_COL32(0xFF, 0xFF, 0xFF, 0xFF);
            bool bShouldDraw = false;

            switch (ActorType) {
            case EType::Forage:
                if (Overlay->Forageables[Type][Quality]) {
                    bShouldDraw = true;
                    Color = Overlay->ForageableColors[Type];
                }
                break;
            case EType::Ore:
                if (Overlay->Ores[Type][Variant]) {
                    bShouldDraw = true;
                    Color = Overlay->OreColors[Type];
                }
                break;
            case EType::Players:
                if (Overlay->Singles[static_cast<int>(EOneOffs::Player)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::Player)];
                }
                break;
            case EType::Animal:
                if (Overlay->Animals[Type][Variant]) {
                    bShouldDraw = true;
                    Color = Overlay->AnimalColors[Type][Variant];
                }
                break;
            case EType::Tree:
                if (Overlay->Trees[Type][Variant]) {
                    bShouldDraw = true;
                    Color = Overlay->TreeColors[Type];
                }
                break;
            case EType::Bug:
                if (Overlay->Bugs[Type][Variant][Quality]) {
                    bShouldDraw = true;
                    Color = Overlay->BugColors[Type][Variant];
                }
                break;
            case EType::NPCs:
                if (Overlay->Singles[static_cast<int>(EOneOffs::NPC)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::NPC)];
                }
                break;
            case EType::Loot:
                if (Overlay->Singles[static_cast<int>(EOneOffs::Loot)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::Loot)];
                }
                break;
            case EType::Quest:
                if (Overlay->Singles[static_cast<int>(EOneOffs::Quest)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::Quest)];
                }
                break;
            case EType::RummagePiles:
                if (Overlay->Singles[static_cast<int>(EOneOffs::RummagePiles)]) {
                    if (auto Pile = static_cast<ATimedLootPile*>(Actor)) {
                        const auto ValeriaCharacter = GetValeriaCharacter();
                        if (ValeriaCharacter && Pile->CanGather(ValeriaCharacter) && Pile->bActivated) {
                            bShouldDraw = true;
                            Color = Overlay->SingleColors[static_cast<int>(EOneOffs::RummagePiles)];
                        }
                        else if (Overlay->bVisualizeDefault) {
                            bShouldDraw = true;
                            Color = Pile->bActivated ? IM_COL32(0xFF, 0xFF, 0xFF, 0xFF) : IM_COL32(0xFF, 0x00, 0x00, 0xFF);
                        }
                    }
                }
                break;
            case EType::Stables:
                if (Overlay->Singles[static_cast<int>(EOneOffs::Stables)]) {
                    bShouldDraw = true;
                    Color = Overlay->SingleColors[static_cast<int>(EOneOffs::Stables)];
                }
                break;
            case EType::Fish:
                if (Overlay->Fish[Type]) {
                    bShouldDraw = true;
                    Color = Overlay->FishColors[Type];
                }
                break;
            default:
                break;
            }

            if (Overlay->bVisualizeDefault && Type == 0)
                bShouldDraw = true;

            if (!bShouldDraw)
                continue;

            if (!Roboto) {
                Roboto = reinterpret_cast<UFont*>(UObject::FindObject("Font Roboto.Roboto", EClassCastFlags::None));

                if (!Roboto)
                    continue;
            }

            // Construct text string
            std::string qualityName = (Quality > 0) ? PaliaOverlay::GetQualityName(Quality, ActorType) : "";

            // Prepare text with optional parts depending on the index values
            std::string text = DisplayName;
            if (!qualityName.empty()) {
                text += " [" + qualityName + "]";
            }
            text += std::format(" [{:.2f}m]", Distance);
            std::wstring wideText(text.begin(), text.end());

            double BaseScale = 1.0; // Default scale at a reference distance
            double ReferenceDistance = 100.0; // Distance at which no scaling is applied
            double ScalingFactor = 0.005; // Determines how much the scale changes with distance

            double DistanceScale;
            DistanceScale = BaseScale - ScalingFactor * (Distance - ReferenceDistance);
            DistanceScale = CustomMath::Clamp(DistanceScale, 0.5, BaseScale); // Clamp the scale to a reasonable range

            const FVector2D TextScale = { DistanceScale * PaliaOverlay::ESPTextScale, DistanceScale * PaliaOverlay::ESPTextScale };
            ImColor IMC(Color);
            FLinearColor TextColor = { IMC.Value.x, IMC.Value.y, IMC.Value.z, IMC.Value.w };

            // Setup shadow properties
            ImColor IMCS(Color);
            FLinearColor ShadowColor = { IMCS.Value.x, IMCS.Value.y, IMCS.Value.z, IMCS.Value.w };

            // Calculate positions
            FVector2D TextPosition = ScreenLocation;
            FVector2D ShadowPosition = { TextPosition.X + 1.0, TextPosition.Y + 1.0 };

            // Draw shadow text
            HUD->Canvas->K2_DrawText(Roboto, FString(wideText.data()), ShadowPosition, TextScale, TextColor, 0, { 0, 0, 0, 1 }, { 1.0f, 1.0f }, true, true, true, { 0, 0, 0, 1 });

            // Draw main text
            HUD->Canvas->K2_DrawText(Roboto, FString(wideText.data()), TextPosition, TextScale, ShadowColor, 0, { 0, 0, 0, 1 }, { 1.0f, 1.0f }, true, true, true, { 0, 0, 0, 1 });
        }
    }

    // Logic for FOV and Targeting Drawing
    if (Overlay->bDrawFOVCircle) {
        FVector2D PlayerScreenPosition;
        FVector2D TargetScreenPosition;

        if (PlayerController->ProjectWorldLocationToScreen(PawnLocation, &PlayerScreenPosition, true)) {
            // Calculate the center of the FOV circle based on the player's screen position
            FVector2D FOVCenter = { HUD->Canvas->ClipX * 0.5f, HUD->Canvas->ClipY * 0.5f };
            DrawCircle(HUD->Canvas, Overlay->FOVRadius, 1200, { 0.485f, 0.485f, 0.485f, 0.485f }, 1.0f);

            if (Overlay->BestTargetLocation.IsZero())
                return;
            if (!PlayerController->ProjectWorldLocationToScreen(Overlay->BestTargetLocation, &TargetScreenPosition, true))
                return;
            if (!(CustomMath::DistanceBetweenPoints(TargetScreenPosition, FOVCenter) <= Overlay->FOVRadius))
                return;

            HUD->Canvas->K2_DrawLine(FOVCenter, TargetScreenPosition, 0.5f, { 0.485f, 0.485f, 0.485f, 0.485f });
        }
    }
}

// [Movement]

inline void Func_DoNoClip(PaliaOverlay* Overlay) {
    if (!Overlay->bEnableNoclip && Overlay->bEnableNoclip == Overlay->bPreviousNoclipState)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    UValeriaCharacterMoveComponent* ValeriaMovementComponent = ValeriaCharacter->GetValeriaCharacterMovementComponent();
    if (!ValeriaMovementComponent || !ValeriaMovementComponent->IsValidLowLevel() || ValeriaMovementComponent->IsDefaultObject())
        return;

    if (Overlay->bEnableNoclip != Overlay->bPreviousNoclipState) {
        if (Overlay->bEnableNoclip) {
            ValeriaMovementComponent->SetMovementMode(EMovementMode::MOVE_Flying, 5);
            ValeriaCharacter->CapsuleComponent->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Ignore);
        }
        else {
            ValeriaMovementComponent->SetMovementMode(EMovementMode::MOVE_Walking, 1);
            ValeriaCharacter->CapsuleComponent->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
            ValeriaCharacter->CapsuleComponent->SetCollisionResponseToChannel(ECollisionChannel::ECC_Pawn, ECollisionResponse::ECR_Block);
        }

        Overlay->bPreviousNoclipState = Overlay->bEnableNoclip;
    }

    // Logic for Noclip Camera
    if (Overlay->bEnableNoclip) {
        if (Overlay->ShowOverlay() || !IsGameWindowActive())
            return;

        const auto PlayerController = GetPlayerController();
        if (!PlayerController)
            return;

        // Calculate forward and right vectors based on the camera's yaw
        const FRotator& CameraRot = PlayerController->PlayerCameraManager->GetCameraRotation();

        FVector CameraForward = UKismetMathLibrary::GetForwardVector(CameraRot);
        FVector CameraRight = UKismetMathLibrary::GetRightVector(CameraRot);
        constexpr FVector CameraUp = { 0.f, 0.f, 1.f };

        CameraForward.Normalize();
        CameraRight.Normalize();

        FVector MovementDirection = { 0.f, 0.f, 0.f };
        constexpr float FlySpeed = 800.0f;

        if (IsKeyHeld('W')) {
            MovementDirection += CameraForward * FlySpeed;
        }
        if (IsKeyHeld('S')) {
            MovementDirection -= CameraForward * FlySpeed;
        }
        if (IsKeyHeld('D')) {
            MovementDirection += CameraRight * FlySpeed;
        }
        if (IsKeyHeld('A')) {
            MovementDirection -= CameraRight * FlySpeed;
        }
        if (IsKeyHeld(VK_SPACE)) {
            MovementDirection += CameraUp * FlySpeed;
        }
        if (IsKeyHeld(VK_CONTROL)) {
            MovementDirection -= CameraUp * FlySpeed;
        }

        // Normalize the total movement direction
        MovementDirection.Normalize();
        MovementDirection *= FlySpeed;

        // Time delta
        constexpr float DeltaTime = 1.0f / 60.0f; // Assuming 60 FPS

        const FVector MovementDelta = MovementDirection * DeltaTime;

        // Update character position
        FHitResult HitResult;
        ValeriaCharacter->K2_SetActorLocation(ValeriaCharacter->K2_GetActorLocation() + MovementDelta, false, &HitResult, false);
    }
}

inline void Func_DoPersistentMovement(const PaliaOverlay* Overlay) {
    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    UValeriaCharacterMoveComponent* ValeriaMovementComponent = ValeriaCharacter->GetValeriaCharacterMovementComponent();
    if (!ValeriaMovementComponent)
        return;

    ValeriaMovementComponent->MaxWalkSpeed = Overlay->CustomWalkSpeed;
    ValeriaMovementComponent->SprintSpeedMultiplier = Overlay->CustomSprintSpeedMultiplier;
    ValeriaMovementComponent->ClimbingSpeed = Overlay->CustomClimbingSpeed;
    ValeriaMovementComponent->GlidingMaxSpeed = Overlay->CustomGlidingSpeed;
    ValeriaMovementComponent->GlidingFallSpeed = Overlay->CustomGlidingFallSpeed;
    ValeriaMovementComponent->JumpZVelocity = Overlay->CustomJumpVelocity;
    ValeriaMovementComponent->MaxStepHeight = Overlay->CustomMaxStepHeight;
}

// [Placement]

inline void Func_DoPlaceAnywhere(const PaliaOverlay* Overlay) {
    if (!Overlay->bPlaceAnywhere)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    UPlacementComponent* PlacementComponent = ValeriaCharacter->GetPlacement();
    if (PlacementComponent) {
        PlacementComponent->CanPlaceHere = true;
    }
}

// [Fishing]

void DetourManager::ToggleFishingDelays(const bool RemoveDelays) {
    UValeriaGameInstance* ValeriaGameInstance = GetValeriaController()->GameInst;
    if (!ValeriaGameInstance || !ValeriaGameInstance->IsValidLowLevel() || ValeriaGameInstance->IsDefaultObject()) {
        return;
    }

    auto& CastSettings = ValeriaGameInstance->Configs.Globals.Fishing->CastSettings;

    float newCastDelay = RemoveDelays ? 0.0f : 0.150f;
    if (std::abs(CastSettings.CastDelay - newCastDelay) < 0.0001f) {
        return;
    }

    auto& FishingSettings = ValeriaGameInstance->Configs.Globals.Fishing;
    auto& EndSettings = ValeriaGameInstance->Configs.Globals.Fishing->EndSettings;

    CastSettings.CastDelay = newCastDelay;
    CastSettings.MaxDistanceToCast = 1500.0f; // Default value
    CastSettings.MinDistanceToCast = RemoveDelays ? 1500.0f : 500.0f;
    CastSettings.LaunchOffset = RemoveDelays ? FVector{ 1500, 0, -300 } : FVector{};
    CastSettings.WindupSpeed = RemoveDelays ? FLT_MAX : 0.350f;

    FishingSettings->FishingFinishReelInSpeed = RemoveDelays ? FLT_MAX : 1600.0f;
    FishingSettings->TotalCelebrationDuration = RemoveDelays ? 0.0f : 4.0f;
    FishingSettings->OnBeginReelingInitialCooldown = RemoveDelays ? 0.0f : 1.0f;

    EndSettings.MaxTimeOfEndFishingCelebrate = RemoveDelays ? 0.0f : 4.5f;
    EndSettings.MaxTimeOfEndFishingDefault = RemoveDelays ? 0.0f : 2.0f;
    EndSettings.MaxTimeOfEndFishingEmptyHanded = RemoveDelays ? 0.0f : 2.2f;
    EndSettings.MaxTimeOfEndFishingFailure = RemoveDelays ? 0.0f : 1.75f;
}

inline void Func_DoInstantCatch(const PaliaOverlay* Overlay) {
    if (!Overlay->bFishingInstantCatch)
        return;

    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaCharacter)
        return;

    UFishingComponent* FishingComponent = ValeriaCharacter->GetFishing();
    if (!FishingComponent)
        return;

    if (static_cast<EFishingState_NEW>(FishingComponent->GetFishingState()) == EFishingState_NEW::Bite) {
        FFishingEndContext Context;
        FishingComponent->RpcServer_EndFishing(Context);
        FishingComponent->SetFishingState(EFishingState_OLD::None);
    }
}

inline void Func_DoFishingActivities(const PaliaOverlay* Overlay) {
    const auto ValeriaController = GetValeriaController();
    const auto ValeriaCharacter = GetValeriaCharacter();
    if (!ValeriaController || !ValeriaCharacter) {
        return;
    }

    // Auto Fishing

    // This only works when there is no delay to fishing (Animations)
    // And as this event (function) happens AFTER the fishing already ended
    // We can just cast again, also above the inventory-loop aspect for faster fishing.
    if (Overlay->bEnableAutoFishing) {
        if (ValeriaCharacter->GetEquippedItem().ItemType->Name.ToString().find("Tool_Rod_") != std::string::npos) {
            ValeriaCharacter->ToolPrimaryActionPressed();
            ValeriaCharacter->ToolPrimaryActionReleased();
        }
    }

    // Avoid doing extra work
    if (!Overlay->bFishingSell && !Overlay->bFishingDiscard && !Overlay->bFishingOpenStoreWaterlogged) {
        return;
    }

    UVillagerStoreComponent* StoreComponent = ValeriaCharacter->StoreComponent;
    const UInventoryComponent* InventoryComponent = ValeriaCharacter->GetInventory();
    if (!InventoryComponent) {
        return;
    }

    // Sell / Discard / Storage
    for (int BagIndex = 0; BagIndex < InventoryComponent->Bags.Num(); BagIndex++) {
        for (int SlotIndex = 0; SlotIndex < 8; SlotIndex++) {
            FBagSlotLocation Slot{ BagIndex, SlotIndex };
            FValeriaItem Item = InventoryComponent->GetItemAt(Slot);

            if (Overlay->bFishingSell && Item.ItemType->Category == EItemCategory::Fish && StoreComponent) {
                StoreComponent->RpcServer_SellItem(Slot, 10);
            }
            else if (Overlay->bFishingDiscard && Item.ItemType->Category == EItemCategory::Junk) {
                // Don't ever discard more than the amount of the stack
                ValeriaController->DiscardItem(Slot, Item.Amount);
            }
            else if (Item.ItemType->PersistId == 2810) { // Waterlogged Chest
                if (!Overlay->bFishingOpenStoreWaterlogged) {
                    // Don't ever discard more than the amount of the stack
                    ValeriaController->DiscardItem(Slot, Item.Amount);
                }
                else {
                    ValeriaController->ConsumeItem(Slot);
                }
            }
            else if (Overlay->bFishingOpenStoreWaterlogged && Item.ItemType->Name.ToString().find("DA_ItemType_Decor_Makeshift_") != std::string::npos) {
                ValeriaController->MoveItemSlotToStorage(Slot, 1, EStoragePoolType::Primary);
            }
        }
    }
}

inline void Func_DoFishingCaptureOverride(PaliaOverlay* Overlay, Params::FishingComponent_RpcServer_SelectLoot* SelectLoot) {
    if (Overlay->bCaptureFishingSpot) {
        memcpy(&Overlay->sOverrideFishingSpot, &SelectLoot->RPCLootParams.WaterType_Deprecated, sizeof(FName));
        Overlay->bCaptureFishingSpot = false;
    }
    if (Overlay->bOverrideFishingSpot) {
        memcpy(&SelectLoot->RPCLootParams.WaterType_Deprecated, &Overlay->sOverrideFishingSpot, sizeof(FName));
    }
}

Params::FishingComponent_RpcServer_EndFishing* EndFishingDetoured(const PaliaOverlay* Overlay, Params::FishingComponent_RpcServer_EndFishing* EndFishing) {
    Params::FishingComponent_RpcServer_EndFishing* EndFishingDetoured = EndFishing;

    if (Overlay->bFishingInstantCatch) {
        EndFishingDetoured->Context.Result = EFishingMiniGameResult::Success;
    }

    if (Overlay->bFishingPerfectCatch) {
        EndFishingDetoured->Context.Perfect = Overlay->bFishingPerfectCatch;
    }

    if (Overlay->bFishingNoDurability) {
        EndFishingDetoured->Context.DurabilityReduction = 0;
    }

    EndFishing->Context.SourceWaterBody = Overlay->fWaterBody && Overlay->fWaterBody->IsValidLowLevel() ? Overlay->fWaterBody : nullptr;
    EndFishing->Context.bUsedMultiplayerHelp = Overlay->bFishingMultiplayerHelp;
    EndFishing->Context.StartRodHealth = 100.0f;
    EndFishing->Context.EndRodHealth = 100.0f;
    EndFishing->Context.StartFishHealth = 100.0f;
    EndFishing->Context.EndFishHealth = 100.0f;
    return EndFishingDetoured;
}

// Detouring

void DetourManager::ProcessEventDetour(const UObject* Class, const UFunction* Function, void* Params) {
    const auto Overlay = dynamic_cast<PaliaOverlay*>(OverlayBase::Instance);
    const auto fn = Function->GetFullName();
    invocations.insert(fn);

    // Custom Tick-Safe Method for calling functions only allows within player ticks
    if (fn == "Function Engine.Actor.ReceiveTick") {
        // Custom tick
    }
    else if (fn == "Function Engine.HUD.ReceiveDrawHUD") {
        // [Logic] Draw ESP
        Func_DoESP(Overlay, reinterpret_cast<const AHUD*>(Class));

        // [Logic] InteliTargeting Updates (FOV)
        Func_DoInteliAim(Overlay);

        // [Logic] Persisten Movement
        Func_DoPersistentMovement(Overlay);

        // [Logic] Noclip
        Func_DoNoClip(Overlay);

        // [Logic] Housing Place Anywhere
        Func_DoPlaceAnywhere(Overlay);
    }

    // [Fishing] Fish Pools (Capturing / Overriding)
    if (fn == "Function Palia.FishingComponent.RpcServer_SelectLoot") {
        auto SelectLoot = static_cast<Params::FishingComponent_RpcServer_SelectLoot*>(Params);
        Func_DoFishingCaptureOverride(Overlay, SelectLoot);
    }

    // [Fun] Teleport to Waypoint
    if (fn == "Function Palia.TrackingComponent.RpcClient_SetUserMarkerViaWorldMap") {
        auto SetUserMarkerViaWorldMap = static_cast<Params::TrackingComponent_RpcClient_SetUserMarkerViaWorldMap*>(Params);
        Func_DoTeleportToWaypoint(Overlay, SetUserMarkerViaWorldMap);
    }

    // [Fishing] Instant Catch
    if (fn == "Function Palia.FishingComponent.RpcClient_StartFishingAt_Deprecated") {
        Func_DoInstantCatch(Overlay);
    }

    // [Fishing] End Fishing Detouring (Instant Catch, Always Perfect, No Durability, Multiplayer Help)
    if (fn == "Function Palia.FishingComponent.RpcServer_EndFishing") {
        auto EndFishing = static_cast<Params::FishingComponent_RpcServer_EndFishing*>(Params);
        EndFishing = EndFishingDetoured(Overlay, EndFishing);
    }

    // [Fishing] Fish Caught (Auto Fish / Sell / Discard / Store Items)
    if (fn == "Function Palia.FishingComponent.RpcClient_FishCaught") {
        Func_DoFishingActivities(Overlay);
    }

    // Movement Velocity Logic
    if (fn == "Function Palia.ValeriaClientPriMovementComponent.RpcServer_SendMovement") {
        auto MovementParams = static_cast<Params::ValeriaClientPriMovementComponent_RpcServer_SendMovement*>(Params);

        if (const auto ValeriaCharacter = GetValeriaCharacter()) {
            if (ValeriaCharacter) {
                UValeriaCharacterMoveComponent* ValeriaMovementComponent = ValeriaCharacter->GetValeriaCharacterMovementComponent();
                if (ValeriaMovementComponent) {
                    MovementParams->MoveInfo.TargetVelocity = { 0, 0, 0 };
                }
            }
        }
    }

    // Silent Aim Projectile Logic
    if (fn == "Function Palia.ProjectileFiringComponent.RpcServer_FireProjectile") {
        auto FireProjectile = static_cast<Params::ProjectileFiringComponent_RpcServer_FireProjectile*>(Params);

        const auto ValeriaCharacter = GetValeriaCharacter();

        UProjectileFiringComponent* FiringComponent = nullptr;
        if (ValeriaCharacter) {
            FiringComponent = ValeriaCharacter->GetFiringComponent();
        }

        if (FiringComponent) {
            if (Overlay->bEnableSilentAimbot && Overlay->BestTargetActor) {
                FVector TargetLocation = Overlay->BestTargetActor->K2_GetActorLocation();
                FVector HitLocation = TargetLocation;

                // Find the projectile after it's fired and directly set its hit status
                for (auto& [ProjectileId, Pad_22C8, ProjectileActor, HasHit, Pad_22C9] : FiringComponent->FiredProjectiles) {
                    if (ProjectileId == FireProjectile->ProjectileId) {
                        FVector ProjectileLocation = ProjectileActor->K2_GetActorLocation();
                        FVector FiringTargetLocation = Overlay->BestTargetActor->K2_GetActorLocation();

                        // Calculate a point slightly in front of the target location
                        FVector DirectionToTarget = (FiringTargetLocation - ProjectileLocation).GetNormalized();
                        float DistanceBeforeTarget = 500.0f; // Adjust this distance as needed
                        FVector NewProjectileLocation = FiringTargetLocation - (DirectionToTarget * DistanceBeforeTarget);

                        HasHit = true;
                        FHitResult HitResult;
                        ProjectileActor->K2_SetActorLocation(NewProjectileLocation, false, &HitResult, false);
                        HitResult.Location = { NewProjectileLocation };

                        FiringComponent->RpcServer_NotifyProjectileHit(FireProjectile->ProjectileId, Overlay->BestTargetActor, HitLocation);
                    }
                }
            }
        }
    }

    if (OriginalProcessEvent) {
        OriginalProcessEvent(Class, Function, Params);
    }
}

// Essentials

void DetourManager::SetupDetour(void* Instance, void (*DetourFunc)(const UObject*, const UFunction*, void*)) {
    const void** Vtable = *static_cast<const void***>(Instance);

    DWORD OldProtection;
    VirtualProtect(Vtable, sizeof(DWORD) * 1024, PAGE_EXECUTE_READWRITE, &OldProtection);

    const int32_t Idx = Offsets::ProcessEventIdx;
    OriginalProcessEvent = reinterpret_cast<void(*)(const UObject*, const UFunction*, void*)>(reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr)) + Offsets::ProcessEvent);
    Vtable[Idx] = DetourFunc;

    HookedClient = Instance;
    VirtualProtect(Vtable, sizeof(DWORD) * 1024, OldProtection, &OldProtection);
}

void DetourManager::SetupDetour(void* Instance) {
    SetupDetour(Instance, &DetourManager::ProcessEventDetour);
}

void DetourManager::ProcessEventDetourCallback(const UObject* Class, const UFunction* Function, void* Params, const DetourManager* manager) {
    manager->ProcessEventDetour(Class, Function, Params);
}