#include "Mesh.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>

using namespace dEngine;

namespace {
	int checks = 0;
	void Check(bool result, const char* message) {
		if (!result) throw std::runtime_error(message);
		++checks;
	}
	bool Equal(const glm::vec3& a, const glm::vec3& b) {
		return a.x == b.x && a.y == b.y && a.z == b.z;
	}
	template<class T, size_t N>
	std::vector<std::byte> Bytes(const T (&data)[N]) {
		const auto bytes = std::as_bytes(std::span(data));
		return { bytes.begin(), bytes.end() };
	}
	Mesh MakeMesh() {
		// binding 7 的位置从 offset 4 读取；最后一个顶点故意不被绘制。
		const float positions[][4] = {
			{42, -2, -1, 0}, {42, 2, -1, 0}, {42, 0, 3, 1},
			{42, 1, 0, -2}, {42, 999, 999, 999}
		};
		const float colors[5][4] = {};
		const uint16_t indices[] = {0, 1, 2};
		Mesh mesh;
		mesh.vertexCount = 5;
		mesh.layout.bindings = {{7, 16, false}, {2, 16, false}};
		mesh.layout.attributes = {
			{VertexSemantic::Position, 0, 7, rhi::VertexFormat::Float3, 4},
			{VertexSemantic::Color, 0, 2, rhi::VertexFormat::Float4, 0}
		};
		mesh.vertexStreams = {{2, Bytes(colors)}, {7, Bytes(positions)}};
		mesh.indices = IndexData{rhi::IndexType::UInt16, Bytes(indices)};
		Submesh indexed;
		indexed.range = IndexedRange{0, 3, 0};
		Submesh direct;
		direct.range = VertexRange{1, 3};
		direct.materialSlot = 1;
		mesh.submeshes = {indexed, direct};
		return mesh;
	}
}

int main() {
	try {
		Mesh mesh = MakeMesh();
		Check(!mesh.Validate(), "uncomputed bounds must be rejected");
		Check(mesh.RecalculateBounds(), "mixed indexed/direct bounds");
		Check(mesh.Validate(), "valid mixed multi-stream mesh");
		Check(mesh.indices->IndexCount() == 3, "UInt16 element count");
		Check(Equal(mesh.localBounds.min, {-2, -1, -2})
			&& Equal(mesh.localBounds.max, {2, 3, 1}), "exclude unused vertices");
		Check(Equal(mesh.submeshes[0].localBounds.min, {-2, -1, 0}), "indexed bounds");
		Check(Equal(mesh.submeshes[1].localBounds.min, {0, -1, -2}), "non-indexed bounds");
		Check(mesh.FindVertexStream(7) == &mesh.vertexStreams[1]
			&& mesh.FindVertexStream(99) == nullptr, "explicit binding lookup");
		const auto indexedDraw = mesh.GetDrawMetadata(0);
		Check(indexedDraw && indexedDraw->indexType == rhi::IndexType::UInt16
			&& indexedDraw->first == 0 && indexedDraw->count == 3 && indexedDraw->baseVertex == 0,
			"query UInt16 indexed command elements");
		const auto vertexDraw = mesh.GetDrawMetadata(1);
		Check(vertexDraw && !vertexDraw->indexType && vertexDraw->first == 1 && vertexDraw->count == 3
			&& vertexDraw->baseVertex == 0 && vertexDraw->materialSlot == 1
			&& Equal(vertexDraw->localBounds.min, mesh.submeshes[1].localBounds.min)
			&& Equal(vertexDraw->localBounds.max, mesh.submeshes[1].localBounds.max),
			"query non-indexed command and culling bounds from a mixed mesh");
		Check(!mesh.GetDrawMetadata(mesh.submeshes.size()), "query rejects invalid submesh index");

		Mesh shifted = MakeMesh();
		const uint32_t shiftedIndices[] = {999, 1, 2, 3};
		shifted.indices = IndexData{rhi::IndexType::UInt32, Bytes(shiftedIndices)};
		shifted.submeshes.resize(1);
		shifted.submeshes[0].range = IndexedRange{1, 3, -1};
		Check(shifted.RecalculateBounds() && shifted.Validate(), "valid negative baseVertex");
		Check(Equal(shifted.localBounds.min, {-2, -1, 0}), "negative offset bounds");
		const auto shiftedDraw = shifted.GetDrawMetadata(0);
		Check(shiftedDraw && shiftedDraw->indexType == rhi::IndexType::UInt32
			&& shiftedDraw->first == 1 && shiftedDraw->count == 3 && shiftedDraw->baseVertex == -1,
			"query keeps firstIndex in elements and baseVertex signed");
		shifted.submeshes[0].range = IndexedRange{1, 3, 1};
		Check(shifted.RecalculateBounds() && shifted.Validate(), "positive baseVertex");
		Check(Equal(shifted.localBounds.max, {999, 999, 999}), "positive offset bounds");

		Mesh direct = MakeMesh();
		direct.submeshes.erase(direct.submeshes.begin());
		direct.indices.reset();
		Check(direct.RecalculateBounds() && direct.Validate(), "pure non-indexed mesh");

		auto Reject = [&](Mesh bad, const char* label) {
			std::vector<std::string> errors{"preserved"};
			Check(!bad.Validate(&errors) && errors.size() > 1 && errors.front() == "preserved", label);
		};
		Mesh bad = mesh;
		bad.submeshes[0].range = IndexedRange{0, 3, -1};
		Reject(bad, "negative vertex address");
		bad = mesh;
		bad.submeshes[0].range = IndexedRange{UINT32_MAX, 3, 0};
		Reject(bad, "index range addition overflow");
		bad = mesh;
		bad.submeshes[1].range = VertexRange{UINT32_MAX, 3};
		Reject(bad, "vertex range addition overflow");
		Check(!bad.GetDrawMetadata(1), "query rejects overflowing vertex range");
		bad = mesh;
		const uint32_t hugeIndices[] = {0, 1, UINT32_MAX};
		bad.indices = IndexData{rhi::IndexType::UInt32, Bytes(hugeIndices)};
		Reject(bad, "large unsigned index");
		bad = mesh;
		bad.indices.reset();
		Reject(bad, "missing index data");
		Check(!bad.GetDrawMetadata(0) && bad.GetDrawMetadata(1), "only indexed query requires index data");
		bad = mesh;
		bad.indices->bytes.pop_back();
		Check(bad.indices->IndexCount() == 0, "partial index count");
		Reject(bad, "partial index bytes");
		Check(!bad.GetDrawMetadata(0), "query rejects incomplete index storage");
		bad = mesh;
		bad.indices->type = static_cast<rhi::IndexType>(999);
		Reject(bad, "invalid index enum without division by zero");
		bad = mesh;
		bad.vertexStreams[0].bytes.pop_back();
		Reject(bad, "truncated stream");
		bad = mesh;
		bad.layout.bindings[0].perInstance = true;
		Reject(bad, "per-instance stream rejected");
		bad = mesh;
		bad.layout.attributes[0].offset = UINT32_MAX - 3;
		Reject(bad, "attribute offset overflow");
		bad = mesh;
		bad.layout.attributes[1] = bad.layout.attributes[0];
		Reject(bad, "duplicate attribute semantic");
		bad = mesh;
		bad.vertexStreams[0].binding = 7;
		Reject(bad, "duplicate and missing stream bindings");
		bad = mesh;
		bad.localBounds.min.x = 0;
		Reject(bad, "mesh bounds must contain submesh bounds");
		bad = mesh;
		bad.submeshes[0].range = IndexedRange{0, 2, 0};
		Reject(bad, "incomplete triangle");
		bad = mesh;
		bad.submeshes[0].topology = rhi::PrimitiveTopology::PatchList;
		Check(bad.Validate(), "patch control point count");
		bad.submeshes[0].patchControlPoints = 0;
		Reject(bad, "zero patch control point count");
		Reject(Mesh{}, "empty mesh");

		bad = mesh;
		bad.layout.attributes.erase(bad.layout.attributes.begin());
		Check(!bad.RecalculateBounds(), "missing position semantic");
		Reject(bad, "position required even with valid stored bounds");
		bad = mesh;
		bad.layout.attributes[0].format = rhi::VertexFormat::Float2;
		Check(!bad.RecalculateBounds(), "position format contract");

		// 第二个子网格中的 NaN 不得改动第一个子网格和全局包围盒。
		bad = mesh;
		const float nan = std::numeric_limits<float>::quiet_NaN();
		std::memcpy(bad.vertexStreams[1].bytes.data() + 3 * 16 + 4, &nan, sizeof(nan));
		std::string error;
		Check(!bad.RecalculateBounds(&error) && !error.empty(), "non-finite position");
		Reject(bad, "Validate rejects non-finite position");
		Check(Equal(bad.localBounds.min, mesh.localBounds.min)
			&& Equal(bad.submeshes[0].localBounds.max, mesh.submeshes[0].localBounds.max),
			"failed bounds calculation is transactional");

		Check(mesh.FindAttribute(VertexSemantic::Position) == &mesh.layout.attributes[0]
			&& mesh.FindAttribute(VertexSemantic::Color) == &mesh.layout.attributes[1]
			&& mesh.FindAttribute(VertexSemantic::TexCoord) == nullptr
			&& mesh.FindAttribute(VertexSemantic::Position, 1) == nullptr,
			"attribute lookup uses semantic and channel");
		bad = mesh;
		std::swap(bad.layout.attributes[0], bad.layout.attributes[1]);
		Check(bad.RecalculateBounds() && bad.Validate()
			&& Equal(bad.localBounds.min, mesh.localBounds.min), "position need not be first attribute");
		bad = mesh;
		bad.vertexStreams[0].bytes.resize(4 * 16);
		Reject(bad, "all streams must cover the shared vertex count even when extra vertex is unused");
		bad = mesh;
		bad.vertexStreams[0].bytes.resize(6 * 16);
		Reject(bad, "extra stream vertices are rejected");
		bad = mesh;
		bad.vertexCount = 0;
		Reject(bad, "zero shared vertex count");
		bad = mesh;
		bad.vertexCount = UINT32_MAX;
		Reject(bad, "shared count byte multiplication must not overflow");
		bad = mesh;
		bad.layout.attributes[1].semantic = static_cast<VertexSemantic>(999);
		Reject(bad, "unknown semantic");
		bad = mesh;
		bad.layout.attributes[0].semanticIndex = 1;
		Reject(bad, "Position1 is not a second position set");

		// 模拟导入得到的法线、带符号切线、多 UV 和稀疏颜色通道。
		Mesh channels = mesh;
		float extras[5][16] = {};
		for (auto& vertex : extras) {
			vertex[1] = 1;  // normal.y
			vertex[3] = 1;  // tangent.x
			vertex[6] = 1;  // tangent.w
			for (size_t c = 12; c < 16; ++c) vertex[c] = 1; // color
		}
		channels.layout.bindings[1].stride = 64;
		channels.vertexStreams[0].bytes = Bytes(extras);
		channels.layout.attributes = {
			{VertexSemantic::TexCoord, 1, 2, rhi::VertexFormat::Float3, 36},
			{VertexSemantic::Normal, 0, 2, rhi::VertexFormat::Float3, 0},
			{VertexSemantic::Position, 0, 7, rhi::VertexFormat::Float3, 4},
			{VertexSemantic::Tangent, 0, 2, rhi::VertexFormat::Float4, 12},
			{VertexSemantic::TexCoord, 0, 2, rhi::VertexFormat::Float2, 28},
			{VertexSemantic::Color, 3, 2, rhi::VertexFormat::Float4, 48}
		};
		Check(channels.RecalculateBounds() && channels.Validate(), "optional channels with shared vertex domain");
		Check(channels.FindAttribute(VertexSemantic::TexCoord, 1) == &channels.layout.attributes[0]
			&& channels.FindAttribute(VertexSemantic::Color) == nullptr
			&& channels.FindAttribute(VertexSemantic::Color, 3) != nullptr,
			"multiple UV and sparse color channels are preserved");
		bad = channels;
		bad.layout.attributes[4].format = rhi::VertexFormat::Float;
		Check(bad.Validate(), "one-component texture coordinate");
		bad = channels;
		bad.layout.attributes[5].format = rhi::VertexFormat::Float3;
		Check(bad.Validate(), "RGB vertex color");
		bad = channels;
		bad.layout.attributes[4].semanticIndex = 1;
		Reject(bad, "duplicate UV channel");
		bad = channels;
		bad.layout.attributes[1].semanticIndex = 1;
		Reject(bad, "normal semantic has only channel zero");
		bad = channels;
		bad.layout.attributes[1].format = rhi::VertexFormat::Float2;
		Reject(bad, "normal needs three components");
		bad = channels;
		bad.layout.attributes[3].format = rhi::VertexFormat::Float3;
		Reject(bad, "tangent needs handedness component");
		bad = channels;
		bad.layout.attributes[4].format = rhi::VertexFormat::Float4;
		Reject(bad, "texture coordinate supports up to three components");
		bad = channels;
		bad.layout.attributes[5].format = rhi::VertexFormat::Float2;
		Reject(bad, "color needs RGB or RGBA");

		bad = mesh;
		bad.localBounds = {};
		bad.localBounds.Expand(glm::vec3(0));
		for (auto& submesh : bad.submeshes) submesh.localBounds = bad.localBounds;
		Reject(bad, "nested but incorrect zero bounds are rejected");
		Check(bad.RecalculateBounds() && bad.Validate(), "recalculation repairs stale bounds");
		bad = mesh;
		bad.submeshes[0].localBounds.min.x = 0;
		Reject(bad, "submesh must cover its referenced positions");
		bad = mesh;
		bad.localBounds.min = {-1000, -1000, -1000};
		bad.localBounds.max = {1000, 1000, 1000};
		for (auto& submesh : bad.submeshes) submesh.localBounds = bad.localBounds;
		Check(bad.Validate(), "conservatively oversized bounds are allowed");
		const float movedX = 100;
		bad = mesh;
		std::memcpy(bad.vertexStreams[1].bytes.data() + 4, &movedX, sizeof(movedX));
		Reject(bad, "vertex mutation invalidates old bounds");
		error = "old error";
		Check(bad.RecalculateBounds(&error) && error.empty() && bad.Validate()
			&& bad.localBounds.max.x == movedX, "successful recalculation clears error and updates geometry bounds");
		bad = mesh;
		std::memcpy(bad.vertexStreams[1].bytes.data() + 4 * 16 + 4, &nan, sizeof(nan));
		Reject(bad, "unused position must still be finite");
		Check(!bad.RecalculateBounds(), "non-finite unused position prevents finalization");
		const float inf = std::numeric_limits<float>::infinity();
		bad = mesh;
		std::memcpy(bad.vertexStreams[1].bytes.data() + 4, &inf, sizeof(inf));
		Reject(bad, "infinite position is rejected");
		bad = mesh;
		const uint32_t paddedIndices[] = {UINT32_MAX, 0, 1, 2, UINT32_MAX};
		bad.indices = IndexData{rhi::IndexType::UInt32, Bytes(paddedIndices)};
		bad.submeshes[0].range = IndexedRange{1, 3, 0};
		Check(bad.RecalculateBounds() && bad.Validate()
			&& Equal(bad.submeshes[0].localBounds.max, mesh.submeshes[0].localBounds.max),
			"only indices inside the submesh range are interpreted");

		Aabb box;
		Check(!box.IsValid() && !box.Contains(glm::vec3(0)), "default bounds are empty");
		Check(box.Expand({1, 2, 3}) && box.IsValid() && box.Contains(glm::vec3(1, 2, 3)),
			"degenerate bounds are valid");
		Check(!box.Expand({nan, 0, 0}) && Equal(box.min, {1, 2, 3}), "reject non-finite point");
		std::cout << "Mesh checks passed: " << checks << '\n';
	}
	catch (const std::exception& error) {
		std::cerr << "Mesh check failed: " << error.what() << '\n';
		return 1;
	}
}
