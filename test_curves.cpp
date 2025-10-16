#include "src/o3ds/model.h"
#include <iostream>
#include <cassert>

// Simple test to verify curve serialization/deserialization works
int main()
{
    O3DS::SubjectList subjects;
    
    // Create a subject with some curves
    auto subject = subjects.addSubject("TestSubject");
    subject->mCurveNames.push_back("EyeBrowUp_L");
    subject->mCurveNames.push_back("EyeBrowUp_R");
    subject->mCurveNames.push_back("Smile");
    
    subject->mCurveValues.push_back(0.5f);
    subject->mCurveValues.push_back(0.3f);
    subject->mCurveValues.push_back(0.8f);
    
    // Add a simple transform (required for a valid subject)
    auto transform = subject->addTransform("Root", -1);
    transform->translation.value.v[0] = 1.0;
    transform->translation.value.v[1] = 2.0;
    transform->translation.value.v[2] = 3.0;
    
    std::cout << "Created subject with " << subject->mCurveNames.size() << " curves" << std::endl;
    
    // Serialize the subject
    std::vector<char> buffer;
    int size = subjects.Serialize(buffer);
    
    std::cout << "Serialized data size: " << size << " bytes" << std::endl;
    
    // Parse it back
    O3DS::SubjectList parsedSubjects;
    bool success = parsedSubjects.Parse(buffer.data(), buffer.size());
    
    if (!success) {
        std::cerr << "Parse failed: " << parsedSubjects.mError << std::endl;
        return 1;
    }
    
    // Verify the curves were preserved
    auto parsedSubject = parsedSubjects.findSubject("TestSubject");
    if (!parsedSubject) {
        std::cerr << "Could not find parsed subject" << std::endl;
        return 1;
    }
    
    std::cout << "Parsed subject has " << parsedSubject->mCurveNames.size() << " curves" << std::endl;
    
    // Check curve data
    assert(parsedSubject->mCurveNames.size() == 3);
    assert(parsedSubject->mCurveValues.size() == 3);
    assert(parsedSubject->mCurveNames[0] == "EyeBrowUp_L");
    assert(parsedSubject->mCurveNames[1] == "EyeBrowUp_R");
    assert(parsedSubject->mCurveNames[2] == "Smile");
    assert(std::abs(parsedSubject->mCurveValues[0] - 0.5f) < 0.001f);
    assert(std::abs(parsedSubject->mCurveValues[1] - 0.3f) < 0.001f);
    assert(std::abs(parsedSubject->mCurveValues[2] - 0.8f) < 0.001f);
    
    std::cout << "Curve data verification passed!" << std::endl;
    
    // Test curve updates
    subject->mCurveValues[1] = 0.7f;  // Change EyeBrowUp_R
    
    size_t count = 0;
    int updateSize = subjects.SerializeUpdate(buffer, count);
    
    std::cout << "Update serialized with " << count << " changes, size: " << updateSize << " bytes" << std::endl;
    
    // Parse the update
    bool updateSuccess = parsedSubjects.Parse(buffer.data(), buffer.size(), nullptr, false);
    
    if (!updateSuccess) {
        std::cerr << "Update parse failed: " << parsedSubjects.mError << std::endl;
        return 1;
    }
    
    // Verify the curve was updated
    assert(std::abs(parsedSubject->mCurveValues[1] - 0.7f) < 0.001f);
    
    std::cout << "Curve update verification passed!" << std::endl;
    std::cout << "All tests passed! Curve support is working." << std::endl;
    
    return 0;
}