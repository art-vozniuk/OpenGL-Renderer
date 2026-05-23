#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace Engine {

	/*
	 * Transform — TRS triple used by every editable scene object.
	 *
	 * Convention: matrix is T * R * S (translation last, scale first, in
	 * column-vector convention). Rotation is a quaternion to avoid
	 * gimbal lock and let interactive rotate-gizmo drags accumulate
	 * cleanly via incremental quat multiplies.
	 *
	 * No parent / hierarchy support yet — every transform is world-space.
	 * The editor only has flat top-level objects for now.
	 */
	struct Transform
	{
		glm::vec3 position = glm::vec3(0.0f);
		glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // (w,x,y,z) identity
		glm::vec3 scale    = glm::vec3(1.0f);

		// Build the 4×4 model matrix on demand. Cheap (one mat3-from-quat
		// + two diagonal compositions) — recomputing per frame is fine.
		glm::mat4 Matrix() const
		{
			glm::mat4 t = glm::translate(glm::mat4(1.0f), position);
			glm::mat4 r = glm::mat4_cast(rotation);
			glm::mat4 s = glm::scale(glm::mat4(1.0f), scale);
			return t * r * s;
		}

		// Apply an incremental local-space translation along world axis k.
		void TranslateAlong(const glm::vec3& worldAxis, float delta)
		{
			position += worldAxis * delta;
		}

		// Apply an incremental rotation around a world axis.
		void RotateAround(const glm::vec3& worldAxis, float radians)
		{
			rotation = glm::normalize(glm::angleAxis(radians, glm::normalize(worldAxis)) * rotation);
		}

		// Multiply scale on a single axis (0=X, 1=Y, 2=Z); pass 3 for uniform.
		void ScaleAxis(int axis, float factor)
		{
			if (axis == 3) {
				scale *= factor;
			} else if (axis >= 0 && axis < 3) {
				scale[axis] *= factor;
			}
		}

		void Reset()
		{
			position = glm::vec3(0.0f);
			rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
			scale    = glm::vec3(1.0f);
		}
	};

}
