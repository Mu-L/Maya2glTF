#include "externals.h"

#include "ExportableMesh.h"
#include "ExportableNode.h"
#include "NodeAnimation.h"
#include "OutputStreamsPatch.h"
#include "Transform.h"

NodeAnimation::NodeAnimation(const ExportableNode &node, const ExportableFrames &frames, const double scaleFactor, const Arguments &arguments)
    : node(node), mesh(node.mesh()), m_scaleFactor(scaleFactor), m_blendShapeCount(mesh ? mesh->blendShapeCount() : 0), m_arguments(arguments) {
    auto &sNode = node.glSecondaryNode();
    auto &pNode = node.glPrimaryNode();

    m_invalidLocalTransformTimes.reserve(10);

    switch (node.transformKind) {
    case TransformKind::Simple:
        m_positions = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::TRANSLATION, 3, false);
        m_rotations = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::ROTATION, 4, false);
        m_scales = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::SCALE, 3, false);
        break;
    case TransformKind::ComplexJoint:
        m_positions = std::make_unique<PropAnimation>(frames, sNode, GLTF::Animation::Path::TRANSLATION, 3, false);
        m_rotations = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::ROTATION, 4, false);
        m_scales = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::SCALE, 3, false);

        m_correctors = std::make_unique<PropAnimation>(frames, sNode, GLTF::Animation::Path::SCALE, 3, false);

        if (m_arguments.forceAnimationChannels) {
            m_dummyProps1 = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::TRANSLATION, 3, false);
            m_dummyProps2 = std::make_unique<PropAnimation>(frames, sNode, GLTF::Animation::Path::ROTATION, 4, false);
        }
        break;

    case TransformKind::ComplexTransform:
        m_positions = std::make_unique<PropAnimation>(frames, sNode, GLTF::Animation::Path::TRANSLATION, 3, false);
        m_rotations = std::make_unique<PropAnimation>(frames, sNode, GLTF::Animation::Path::ROTATION, 4, false);
        m_scales = std::make_unique<PropAnimation>(frames, sNode, GLTF::Animation::Path::SCALE, 3, false);

        m_correctors = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::TRANSLATION, 3, false);

        if (m_arguments.forceAnimationChannels) {
            m_dummyProps1 = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::SCALE, 3, false);
            m_dummyProps2 = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::ROTATION, 4, false);
        }
        break;

    default:
        assert(false);
        break;
    }

    if (m_blendShapeCount > 0) {
        m_weights = std::make_unique<PropAnimation>(frames, pNode, GLTF::Animation::Path::WEIGHTS, m_blendShapeCount, true);
    }
}

void NodeAnimation::sampleAt(const MTime &absoluteTime, const int frameIndex, NodeTransformCache &transformCache) {
    auto &transformState = transformCache.getTransform(&node, m_scaleFactor);
    auto &pTRS = transformState.primaryTRS();
    auto &sTRS = transformState.secondaryTRS();

    if (transformState.maxNonOrthogonality > MAX_NON_ORTHOGONALITY && m_invalidLocalTransformTimes.size() < m_invalidLocalTransformTimes.capacity()) {
        m_maxNonOrthogonality = std::max(m_maxNonOrthogonality, transformState.maxNonOrthogonality);
        m_invalidLocalTransformTimes.emplace_back(absoluteTime);
    }

    switch (node.transformKind) {
    case TransformKind::Simple:
        m_positions->append(gsl::make_span(pTRS.translation));
        m_rotations->appendQuaternion(gsl::make_span(pTRS.rotation));
        m_scales->append(gsl::make_span(pTRS.scale));
        break;
    case TransformKind::ComplexJoint:
        m_positions->append(gsl::make_span(sTRS.translation));
        m_rotations->appendQuaternion(gsl::make_span(pTRS.rotation));
        m_scales->append(gsl::make_span(pTRS.scale));

        m_correctors->append(gsl::make_span(sTRS.scale));

        if (m_arguments.forceAnimationChannels) {
            m_dummyProps1->append(gsl::make_span(pTRS.translation));
            m_dummyProps2->appendQuaternion(gsl::make_span(sTRS.rotation));
        }
        break;

    case TransformKind::ComplexTransform:
        m_positions->append(gsl::make_span(sTRS.translation));
        m_rotations->appendQuaternion(gsl::make_span(sTRS.rotation));
        m_scales->append(gsl::make_span(sTRS.scale));

        m_correctors->append(gsl::make_span(pTRS.translation));

        if (m_arguments.forceAnimationChannels) {
            m_dummyProps1->append(gsl::make_span(pTRS.scale));
            m_dummyProps2->appendQuaternion(gsl::make_span(pTRS.rotation));
        }
        break;

    default:
        assert(false);
        break;
    }

    if (m_blendShapeCount) {
        auto weights = mesh->currentWeights();
        assert(weights.size() == m_blendShapeCount);
        m_weights->append(span(weights));
    }
}

void NodeAnimation::exportTo(GLTF::Animation &glAnimation) {

    const auto detectStepSampleCount = m_arguments.getStepDetectSampleCount();

    if (!m_invalidLocalTransformTimes.empty()) {
        // TODO: Use SVG to decompose the 3x3 matrix into a product of rotation
        // and scale matrices.
        cerr << prefix << "WARNING: node '" << node.name()
             << "' has animated transforms that are not representable by glTF! "
                "Skewing is not supported, use 3 nodes to simulate this. "
                "Largest deviation = "
             << std::fixed << std::setprecision(2) << m_maxNonOrthogonality * 100 << "%" << endl;

        cerr << prefix << "The first invalid transforms were found at times: ";
        for (auto &time : m_invalidLocalTransformTimes)
            cerr << time << " ";
        cerr << endl;
    }

    // Now create the glTF animations, but only for those props that animate
    auto &pTRS = node.initialTransformState.primaryTRS();
    auto &sTRS = node.initialTransformState.secondaryTRS();

    switch (node.transformKind) {
    case TransformKind::Simple:
        finish(glAnimation, "T", m_positions, m_arguments.constantTranslationThreshold, pTRS.translation);
        finish(glAnimation, "R", m_rotations, m_arguments.constantRotationThreshold, pTRS.rotation);
        finish(glAnimation, "S", m_scales, m_arguments.constantScalingThreshold, pTRS.scale);
        break;
    case TransformKind::ComplexJoint:
        finish(glAnimation, "T", m_positions, m_arguments.constantTranslationThreshold, sTRS.translation);
        finish(glAnimation, "R", m_rotations, m_arguments.constantRotationThreshold, pTRS.rotation);
        finish(glAnimation, "S", m_scales, m_arguments.constantScalingThreshold, pTRS.scale);

        finish(glAnimation, "C", m_correctors, m_arguments.constantScalingThreshold, sTRS.scale);

        if (m_arguments.forceAnimationChannels) {
            finish(glAnimation, "DT", m_dummyProps1, 0, pTRS.translation);
            finish(glAnimation, "DR", m_dummyProps2, 0, sTRS.rotation);
        }
        break;

    case TransformKind::ComplexTransform:
        finish(glAnimation, "T", m_positions, m_arguments.constantTranslationThreshold, sTRS.translation);
        finish(glAnimation, "R", m_rotations, m_arguments.constantRotationThreshold, sTRS.rotation);
        finish(glAnimation, "S", m_scales, m_arguments.constantScalingThreshold, sTRS.scale);

        finish(glAnimation, "C", m_correctors, m_arguments.constantScalingThreshold, pTRS.translation);

        if (m_arguments.forceAnimationChannels) {
            finish(glAnimation, "DS", m_dummyProps1, 0, pTRS.scale);
            finish(glAnimation, "DR", m_dummyProps2, 0, pTRS.rotation);
        }
        break;

    default:
        assert(false);
        break;
    }

    if (m_blendShapeCount) {
        const auto initialWeights = mesh->initialWeights();
        assert(initialWeights.size() == m_blendShapeCount);
        finish(glAnimation, "W", m_weights, m_arguments.constantWeightsThreshold, initialWeights);
    }
}

void NodeAnimation::finish(GLTF::Animation &glAnimation, const char *propName, std::unique_ptr<PropAnimation> &animatedProp, double constantThreshold,
                           int detectStepSampleCount, const gsl::span<const float> &baseValues) const {
    const auto dimension = animatedProp->dimension;

    if (dimension) {
        assert(dimension == baseValues.size());

        auto &componentValues = animatedProp->componentValuesPerFrame;

        // Check if all samples are constant. In that case, we drop the animation, unless it is forced
        bool isConstant = true;

        for (size_t offset = 0; offset < componentValues.size() && isConstant; offset += dimension) {
            for (size_t index = 0; index < dimension && isConstant; ++index) {
                isConstant = std::abs(baseValues[index] - componentValues[offset + index]) < constantThreshold;
            }
        }

        if (isConstant && !m_arguments.forceAnimationSampling && !m_arguments.forceAnimationChannels) {
            // All animation frames are the same as the scene, to need to animate the prop.
            animatedProp.release();
        } else {
            const auto useSingleKey = isConstant && !m_arguments.forceAnimationSampling;
            if (useSingleKey || detectStepSampleCount <= 1) {
                animatedProp->finish(m_arguments.disableNameAssignment ? "" : node.name() + "/anim/" + glAnimation.name + "/" + propName, useSingleKey);
                glAnimation.channels.push_back(&animatedProp->glChannel);
            } else {
                // Now detect what parts of the sampled animation could use step-functions.
                //
                // TODO: Apply a curve simplifier.
            }
        }
    }
}

