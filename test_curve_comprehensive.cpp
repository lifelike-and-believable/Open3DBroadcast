#include "src/o3ds/model.h"
#include <iostream>
#include <cassert>
#include <cmath>

void testBasicCurveOperations() {
    std::cout << "Testing basic curve operations..." << std::endl;
    
    O3DS::Subject subject("TestCharacter");
    
    // Add some curves
    subject.mCurveNames = {"EyeBrowUp_L", "EyeBrowUp_R", "Smile", "Frown"};
    subject.mCurveValues = {0.5f, 0.3f, 0.8f, 0.1f};
    
    // Add a minimal transform so the subject is valid
    auto transform = subject.addTransform("Root", -1);
    transform->translation.value.v[0] = 0.0;
    
    assert(subject.mCurveNames.size() == 4);
    assert(subject.mCurveValues.size() == 4);
    std::cout << "✓ Basic curve data setup" << std::endl;
}

void testCurveSerialization() {
    std::cout << "Testing curve serialization..." << std::endl;
    
    O3DS::SubjectList subjects;
    auto subject = subjects.addSubject("FaceCharacter");
    
    // Setup curves
    subject->mCurveNames = {"LeftEyeBlink", "RightEyeBlink", "MouthOpen"};
    subject->mCurveValues = {0.0f, 0.5f, 0.75f};
    
    // Add minimal skeleton
    auto root = subject->addTransform("Root", -1);
    root->translation.value.v[0] = 1.0;
    
    // Serialize
    std::vector<char> buffer;
    int size = subjects.Serialize(buffer);
    
    assert(size > 0);
    assert(buffer.size() > 0);
    std::cout << "✓ Serialization produces " << size << " bytes" << std::endl;
    
    // Parse back
    O3DS::SubjectList parsedSubjects;
    bool success = parsedSubjects.Parse(buffer.data(), buffer.size());
    
    assert(success);
    assert(parsedSubjects.mItems.size() == 1);
    
    auto parsedSubject = parsedSubjects.findSubject("FaceCharacter");
    assert(parsedSubject != nullptr);
    assert(parsedSubject->mCurveNames.size() == 3);
    assert(parsedSubject->mCurveValues.size() == 3);
    
    // Verify curve data
    assert(parsedSubject->mCurveNames[0] == "LeftEyeBlink");
    assert(parsedSubject->mCurveNames[1] == "RightEyeBlink");
    assert(parsedSubject->mCurveNames[2] == "MouthOpen");
    
    assert(std::abs(parsedSubject->mCurveValues[0] - 0.0f) < 0.001f);
    assert(std::abs(parsedSubject->mCurveValues[1] - 0.5f) < 0.001f);
    assert(std::abs(parsedSubject->mCurveValues[2] - 0.75f) < 0.001f);
    
    std::cout << "✓ Curve serialization and parsing" << std::endl;
}

void testCurveUpdates() {
    std::cout << "Testing curve updates..." << std::endl;
    
    O3DS::SubjectList subjects;
    auto subject = subjects.addSubject("AnimatedFace");
    
    // Initial setup
    subject->mCurveNames = {"Joy", "Sadness", "Anger", "Surprise"};
    subject->mCurveValues = {0.2f, 0.1f, 0.0f, 0.3f};
    
    auto root = subject->addTransform("Head", -1);
    
    // Initial serialize
    std::vector<char> buffer;
    subjects.Serialize(buffer);
    
    // Parse into another subject list
    O3DS::SubjectList receiver;
    receiver.Parse(buffer.data(), buffer.size());
    auto receivedSubject = receiver.findSubject("AnimatedFace");
    
    // Verify initial state
    assert(std::abs(receivedSubject->mCurveValues[0] - 0.2f) < 0.001f); // Joy
    assert(std::abs(receivedSubject->mCurveValues[1] - 0.1f) < 0.001f); // Sadness
    
    // Update some curves
    subject->mCurveValues[0] = 0.9f;  // Joy -> 90%
    subject->mCurveValues[2] = 0.6f;  // Anger -> 60%
    
    // Send update
    size_t updateCount = 0;
    int updateSize = subjects.SerializeUpdate(buffer, updateCount);
    
    assert(updateSize > 0);
    assert(updateCount > 0);  // Should include curve updates
    std::cout << "✓ Update contains " << updateCount << " changes" << std::endl;
    
    // Parse update (don't clear existing subjects)
    bool updateSuccess = receiver.Parse(buffer.data(), buffer.size(), nullptr, false);
    assert(updateSuccess);
    
    // Verify updated values
    assert(std::abs(receivedSubject->mCurveValues[0] - 0.9f) < 0.001f); // Joy updated
    assert(std::abs(receivedSubject->mCurveValues[1] - 0.1f) < 0.001f); // Sadness unchanged
    assert(std::abs(receivedSubject->mCurveValues[2] - 0.6f) < 0.001f); // Anger updated
    assert(std::abs(receivedSubject->mCurveValues[3] - 0.3f) < 0.001f); // Surprise unchanged
    
    std::cout << "✓ Curve updates work correctly" << std::endl;
}

void testEmptyCurves() {
    std::cout << "Testing subjects without curves..." << std::endl;
    
    O3DS::SubjectList subjects;
    auto subject = subjects.addSubject("SkeletonOnly");
    
    // No curves, just skeleton
    auto root = subject->addTransform("Root", -1);
    auto child = subject->addTransform("Child", 0);
    
    // Should serialize and parse without issues
    std::vector<char> buffer;
    int size = subjects.Serialize(buffer);
    assert(size > 0);
    
    O3DS::SubjectList parsed;
    bool success = parsed.Parse(buffer.data(), buffer.size());
    assert(success);
    
    auto parsedSubject = parsed.findSubject("SkeletonOnly");
    assert(parsedSubject != nullptr);
    assert(parsedSubject->mCurveNames.empty());
    assert(parsedSubject->mCurveValues.empty());
    assert(parsedSubject->mTransforms.size() == 2);
    
    std::cout << "✓ Subjects without curves work normally" << std::endl;
}

void testMixedContent() {
    std::cout << "Testing mixed skeleton and curve updates..." << std::endl;
    
    O3DS::SubjectList subjects;
    auto subject = subjects.addSubject("FullCharacter");
    
    // Setup both skeleton and curves
    auto root = subject->addTransform("Root", -1);
    root->translation.value.v[0] = 1.0;
    root->translation.value.v[1] = 2.0;
    root->translation.value.v[2] = 3.0;
    
    subject->mCurveNames = {"BlinkLeft", "BlinkRight"};
    subject->mCurveValues = {0.0f, 0.0f};
    
    // Serialize initial state
    std::vector<char> buffer;
    subjects.Serialize(buffer);
    
    O3DS::SubjectList receiver;
    receiver.Parse(buffer.data(), buffer.size());
    auto receivedSubject = receiver.findSubject("FullCharacter");
    
    // Update both transform and curves
    root->translation.value.v[0] = 5.0;  // Move transform
    subject->mCurveValues[0] = 1.0f;     // Close left eye
    subject->mCurveValues[1] = 0.5f;     // Half close right eye
    
    // Send update
    size_t updateCount = 0;
    subjects.SerializeUpdate(buffer, updateCount);
    
    receiver.Parse(buffer.data(), buffer.size(), nullptr, false);
    
    // Verify both transform and curves updated
    assert(std::abs(receivedSubject->mTransforms[0]->translation.value.v[0] - 5.0) < 0.001);
    assert(std::abs(receivedSubject->mCurveValues[0] - 1.0f) < 0.001f);
    assert(std::abs(receivedSubject->mCurveValues[1] - 0.5f) < 0.001f);
    
    std::cout << "✓ Mixed skeleton and curve updates work together" << std::endl;
}

int main() {
    std::cout << "=== Open3DStream Curve Support Tests ===" << std::endl << std::endl;
    
    try {
        testBasicCurveOperations();
        testCurveSerialization();
        testCurveUpdates();
        testEmptyCurves();
        testMixedContent();
        
        std::cout << std::endl << "🎉 All tests passed! Curve support is working correctly." << std::endl;
        std::cout << std::endl << "The Open3DStream protocol now supports:" << std::endl;
        std::cout << "  - Skeletal animation data (existing)" << std::endl;
        std::cout << "  - Animation curve data (NEW)" << std::endl;
        std::cout << "  - Mixed updates of both data types" << std::endl;
        std::cout << "  - Backward compatibility with curve-less subjects" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Test failed with unknown exception" << std::endl;
        return 1;
    }
}