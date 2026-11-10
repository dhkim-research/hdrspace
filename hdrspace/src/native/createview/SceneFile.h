#pragma once

// Reading a Radiance room (.rad) for Create View: primitives, !xform / !include, materials,
// semantic labels, the files the renderer needs, and writing material edits back.

#include "Types.h"

namespace hdrspace::createview {

bool isCreateViewGeometryType(const std::string& type);

std::string inferCreateViewSemanticLabel(const std::string& identifier, const std::string& modifier, const std::string& materialType);

CreateViewPreviewState::Surface resolveCreateViewSurfaceStyle(
    const std::string& identifier,
    const std::string& modifier,
    const std::string& primitiveType,
    const std::map<std::string, CreateViewMaterialInfo>& materials
);

void createViewAddPointSample(
    CreateViewRoomInfo& info,
    const std::array<double, 3>& position,
    const std::array<float, 3>& color,
    float alpha
);

void refreshCreateViewSurfaceStyles(CreateViewRoomInfo& info);

std::vector<fs::path> expandCreateViewPaths(const fs::path& baseDir, const std::string& pattern);

void parseCreateViewPrimitiveTokens(const std::vector<std::string>& tokens, const fs::path& sourcePath, const CreateViewTransform& transform, CreateViewRoomInfo& info);

void parseCreateViewRoomRecursive(const fs::path& path, const CreateViewTransform& transform, std::set<fs::path>& activePaths, CreateViewRoomInfo& info);

std::set<std::string> collectCreateViewUnresolvedModifiers(const CreateViewRoomInfo& info);

std::vector<fs::path> discoverCreateViewSupplementalMaterialFiles(const fs::path& roomPath, const CreateViewRoomInfo& info);

std::vector<fs::path> collectCreateViewRenderDependencyPaths(const CreateViewRoomInfo& roomInfo, const fs::path& roomPath);

void synthesizeCreateViewReferencedMaterials(CreateViewRoomInfo& info);

CreateViewRoomInfo parseCreateViewRoom(const std::string& spec, const fs::path& workingDir);

fs::path writeCreateViewRenderRoomWrapper(const CreateViewRoomInfo& roomInfo, const std::map<std::string, CreateViewMaterialOverride>& materialOverrides, const fs::path& roomPath, const fs::path& wrapperPath, bool includeTemporaryOverrides);

size_t writeCreateViewWindowMaskScene(const CreateViewRoomInfo& roomInfo, const fs::path& scenePath);

bool createViewTypeMatchesSemantic(const std::string& semanticLabel, const std::string& objectType);

bool createViewRadianceTypesAreCompatible(const std::string& lhs, const std::string& rhs);

std::string formatCreateViewMaterialBlock(const CreateViewMaterialInfo& material, const CreateViewMaterialOverride* overrideValue = nullptr);

std::string formatCreateViewMaterialArgLine(const CreateViewMaterialInfo& material, const CreateViewMaterialOverride* overrideValue = nullptr);

bool parseCreateViewMaterialOverrideText(const CreateViewMaterialInfo& material, const std::string& text, CreateViewMaterialOverride& overrideValue, std::string& error);

bool rewriteCreateViewMaterialDefinition(const fs::path& filePath, const CreateViewMaterialInfo& material, const CreateViewMaterialOverride& overrideValue, std::string& error);

} // namespace hdrspace::createview
