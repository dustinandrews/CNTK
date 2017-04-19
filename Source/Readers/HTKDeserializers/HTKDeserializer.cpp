//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "stdafx.h"
#include "HTKDeserializer.h"
#include "ConfigHelper.h"
#include "Basics.h"
#include "StringUtil.h"

// TODO: This will be removed when dependency on old code is eliminated.
// Currently this fixes the linking.
namespace msra { namespace asr {

std::unordered_map<std::wstring, unsigned int> htkfeatreader::parsedpath::archivePathStringMap;
std::vector<std::wstring> htkfeatreader::parsedpath::archivePathStringVector;

}}

namespace Microsoft { namespace MSR { namespace CNTK {

using namespace std;

HTKDeserializer::HTKDeserializer(
    CorpusDescriptorPtr corpus,
    const ConfigParameters& cfg,
    bool primary)
    : DataDeserializerBase(primary),
      m_verbosity(0),
      m_corpus(corpus)
{
    // TODO: This should be read in one place, potentially given by SGD.
    m_frameMode = (ConfigValue)cfg("frameMode", "true");

    m_verbosity = cfg(L"verbosity", 0);

    ConfigParameters input = cfg(L"input");
    auto inputName = input.GetMemberIds().front();
    std::wstring precision = cfg(L"precision", L"float");

    ConfigParameters streamConfig = input(inputName);

    ConfigHelper config(streamConfig);
    auto context = config.GetContextWindow();

    m_expandToPrimary = streamConfig(L"expandToUtterance", false);
    if (m_expandToPrimary && m_primary)
    {
        InvalidArgument("Cannot expand utterances of the primary stream %ls, please change your configuration.", inputName.c_str());
    }

    m_elementType = AreEqualIgnoreCase(precision,  L"float") ? ElementType::tfloat : ElementType::tdouble;
    m_dimension = config.GetFeatureDimension();
    m_dimension = m_dimension * (1 + context.first + context.second);

    InitializeChunkDescriptions(config.GetSequencePaths());
    InitializeStreams(inputName);
    InitializeFeatureInformation();
    InitializeAugmentationWindow(config.GetContextWindow());
}

HTKDeserializer::HTKDeserializer(
    CorpusDescriptorPtr corpus,
    const ConfigParameters& feature,
    const wstring& featureName,
    bool primary)
    : DataDeserializerBase(primary),
      m_corpus(corpus)
{
    // The frame mode is currently specified once per configuration,
    // not in the configuration of a particular deserializer, but on a higher level in the configuration.
    // Because of that we are using find method below.
    m_frameMode = feature.Find("frameMode", "true");

    ConfigHelper config(feature);
    config.CheckFeatureType();

    m_verbosity = feature(L"verbosity", 0);

    auto context = config.GetContextWindow();
    m_elementType = config.GetElementType();

    m_dimension = config.GetFeatureDimension();
    m_dimension = m_dimension * (1 + context.first + context.second);

    m_expandToPrimary = feature(L"expandToUtterance", false);
    if (m_expandToPrimary && m_primary)
    {
        InvalidArgument("Cannot expand utterances of the primary stream %ls, please change your configuration.", featureName.c_str());
    }

    InitializeChunkDescriptions(config.GetSequencePaths());
    InitializeStreams(featureName);
    InitializeFeatureInformation();
    InitializeAugmentationWindow(config.GetContextWindow());
}

void HTKDeserializer::InitializeAugmentationWindow(const std::pair<size_t, size_t>& augmentationWindow)
{
    m_augmentationWindow = augmentationWindow;

    // If not given explicitly, we need to identify the required augmentation range from the expected dimension
    // and the number of dimensions in the file.
    if (m_augmentationWindow.first == 0 && m_augmentationWindow.second == 0)
    {
        const size_t windowFrames = m_dimension / m_ioFeatureDimension; // total number of frames to generate
        const size_t extent = windowFrames / 2;                         // extend each side by this

        if (m_dimension % m_ioFeatureDimension != 0)
            RuntimeError("HTKDeserializer: model vector size is not multiple of input features");

        if (windowFrames % 2 == 0)
            RuntimeError("HTKDeserializer: neighbor expansion of input features to '%zu' is not symmetrical", windowFrames);

        m_augmentationWindow.first = m_augmentationWindow.second = extent;
    }
}

// Initializes chunks based on the configuration and utterance descriptions.
void HTKDeserializer::InitializeChunkDescriptions(const vector<string>& paths)
{
    // Read utterance descriptions.
    vector<UtteranceDescription> utterances;
    utterances.reserve(paths.size());

    string key;
    for (const auto& u : paths)
    {
        key.clear();
        UtteranceDescription description(msra::asr::htkfeatreader::parsedpath::Parse(u, key));

        size_t numberOfFrames = description.GetNumberOfFrames();

        if (m_expandToPrimary && numberOfFrames != 1)
            RuntimeError("Expanded stream should only contain sequences of length 1, utterance '%s' has %zu",
                key.c_str(),
                numberOfFrames);

        if (!m_corpus->IsIncluded(key))
            continue;

        size_t id = m_corpus->KeyToId(key);
        description.SetId(id);
        utterances.push_back(description);
        m_totalNumberOfFrames += numberOfFrames;
    }

    // TODO: We should be able to configure IO chunks based on size.
    // distribute utterances over chunks
    // We simply count off frames until we reach the chunk size.
    // Note that we first randomize the chunks, i.e. when used, chunks are non-consecutive and thus cause the disk head to seek for each chunk.

    // We have 100 frames in a second.
    const size_t FramesPerSec = 100;

    // A chunk constitutes of 15 minutes
    const size_t ChunkFrames = 15 * 60 * FramesPerSec; // number of frames to target for each chunk

    m_chunks.reserve(m_totalNumberOfFrames / ChunkFrames);

    ChunkIdType chunkId = 0;
    foreach_index(i, utterances)
    {
        // if exceeding current entry--create a new one
        // I.e. our chunks are a little larger than wanted (on av. half the av. utterance length).
        if (m_chunks.empty() || m_chunks.back().GetTotalFrames() > ChunkFrames)
        {
            m_chunks.push_back(HTKChunkDescription(chunkId++));
        }

        // append utterance to last chunk
        HTKChunkDescription& currentChunk = m_chunks.back();
        if (!m_primary)
        {
            // Have to store key <-> utterance mapping for non primary deserializers.
            m_keyToChunkLocation.push_back(std::make_tuple(utterances[i].GetId(), currentChunk.GetChunkId(), currentChunk.GetNumberOfUtterances()));
        }

        currentChunk.Add(move(utterances[i]));
    }

    std::sort(m_keyToChunkLocation.begin(), m_keyToChunkLocation.end(),
        [](const std::tuple<size_t, size_t, size_t>& a, const std::tuple<size_t, size_t, size_t>& b)
    {
        return std::get<0>(a) < std::get<0>(b);
    });

    // Check uniqueness.
    for (size_t i = 1; i < m_keyToChunkLocation.size(); ++i)
    {
        if (std::get<0>(m_keyToChunkLocation[i - 1]) == std::get<0>(m_keyToChunkLocation[i]))
            RuntimeError("Please do not use hash as sequence keys, collisions found.");
    }

    fprintf(stderr,
        "HTKDeserializer: selected '%zu' utterances grouped into '%zu' chunks, "
        "average chunk size: %.1f utterances, %.1f frames "
        "(for I/O: %.1f utterances, %.1f frames)\n",
        utterances.size(),
        m_chunks.size(),
        utterances.size() / (double)m_chunks.size(),
        m_totalNumberOfFrames / (double)m_chunks.size(),
        utterances.size() / (double)m_chunks.size(),
        m_totalNumberOfFrames / (double)m_chunks.size());

    if (utterances.empty())
    {
        RuntimeError("HTKDeserializer: No utterances to process.");
    }
}

// Describes exposed stream - a single stream of htk features.
void HTKDeserializer::InitializeStreams(const wstring& featureName)
{
    StreamDescriptionPtr stream = make_shared<StreamDescription>();
    stream->m_id = 0;
    stream->m_name = featureName;
    stream->m_sampleLayout = make_shared<TensorShape>(m_dimension);
    stream->m_elementType = m_elementType;
    stream->m_storageType = StorageType::dense;
    m_streams.push_back(stream);
}

// Reading information about the features from the first file.
// This information is used later to check that all features among all files have the same properties.
void HTKDeserializer::InitializeFeatureInformation()
{
    msra::util::attempt(5, [&]()
    {
        msra::asr::htkfeatreader reader;
        reader.getinfo(m_chunks.front().GetUtterance(0)->GetPath(), m_featureKind, m_ioFeatureDimension, m_samplePeriod);
        fprintf(stderr, "HTKDeserializer: determined feature kind as '%zu'-dimensional '%s' with frame shift %.1f ms\n",
            m_ioFeatureDimension, m_featureKind.c_str(), m_samplePeriod / 1e4);
    });
}

// Gets information about available chunks.
ChunkDescriptions HTKDeserializer::GetChunkDescriptions()
{
    ChunkDescriptions chunks;
    chunks.reserve(m_chunks.size());

    for (ChunkIdType i = 0; i < m_chunks.size(); ++i)
    {
        auto cd = make_shared<ChunkDescription>();
        cd->m_id = i;
        cd->m_numberOfSamples = m_chunks[i].GetTotalFrames();
        // In frame mode, each frame is represented as sequence.
        // The augmentation is still done for frames in the same sequence only, please see GetSequenceById method.
        cd->m_numberOfSequences = m_frameMode ? m_chunks[i].GetTotalFrames() : m_chunks[i].GetNumberOfUtterances();
        chunks.push_back(cd);
    }
    return chunks;
}

// Gets sequences for a particular chunk.
// This information is used by the randomizer to fill in current windows of sequences.
void HTKDeserializer::GetSequencesForChunk(ChunkIdType chunkId, vector<SequenceDescription>& result)
{
    const HTKChunkDescription& chunk = m_chunks[chunkId];
    result.reserve(m_frameMode ? chunk.GetTotalFrames() : chunk.GetNumberOfUtterances());
    size_t offsetInChunk = 0;
    for (size_t i = 0; i < chunk.GetNumberOfUtterances(); ++i)
    {
        auto utterance = chunk.GetUtterance(i);
        // Currently we do not support common prefix, so simply assign the minor to the key.
        size_t sequence = utterance->GetId();

        if (m_frameMode)
        {
            // Because it is a frame mode, creating a sequence for each frame.
            for (size_t k = 0; k < utterance->GetNumberOfFrames(); ++k)
            {
                SequenceDescription f;
                f.m_chunkId = chunkId;
                f.m_key.m_sequence = sequence;
                f.m_key.m_sample = k;
                f.m_indexInChunk = offsetInChunk++;
                f.m_numberOfSamples = 1;
                result.push_back(f);
            }
        }
        else
        {
            // Creating sequence description per utterance.
            SequenceDescription f;
            f.m_chunkId = chunkId;
            f.m_key.m_sequence = sequence;
            f.m_key.m_sample = 0;
            f.m_indexInChunk = offsetInChunk++;
            if (SEQUENCELEN_MAX < utterance->GetNumberOfFrames())
            {
                RuntimeError("Maximum number of samples per sequence exceeded");
            }

            f.m_numberOfSamples = (uint32_t) utterance->GetNumberOfFrames();
            result.push_back(f);
        }
    }
}

// A wrapper around a matrix that views it as a vector of column vectors.
// Does not have any memory associated.
class MatrixAsVectorOfVectors : boost::noncopyable
{
public:
    MatrixAsVectorOfVectors(msra::dbn::matrixbase& m)
        : m_matrix(m)
    {
    }

    size_t size() const
    {
        return m_matrix.cols();
    }

    const_array_ref<float> operator[](size_t j) const
    {
        return array_ref<float>(&m_matrix(0, j), m_matrix.rows());
    }

private:
    msra::dbn::matrixbase& m_matrix;
};


// Represents a chunk data in memory. Given up to the randomizer.
// It is up to the randomizer to decide when to release a particular chunk.
class HTKDeserializer::HTKChunk : public Chunk, boost::noncopyable
{
public:
    HTKChunk(HTKDeserializer* parent, ChunkIdType chunkId) : m_parent(parent), m_chunkId(chunkId)
    {
        auto& chunkDescription = m_parent->m_chunks[chunkId];

        // possibly distributed read
        // making several attempts
        msra::util::attempt(5, [&]()
        {
            chunkDescription.RequireData(m_parent->m_featureKind, m_parent->m_ioFeatureDimension, m_parent->m_samplePeriod, m_parent->m_verbosity);
        });
    }

    // Gets data for the sequence.
    virtual void GetSequence(size_t sequenceId, vector<SequenceDataPtr>& result) override
    {
        m_parent->GetSequenceById(m_chunkId, sequenceId, result);
    }

    // Unloads the data from memory.
    ~HTKChunk()
    {
        auto& chunkDescription = m_parent->m_chunks[m_chunkId];
        chunkDescription.ReleaseData(m_parent->m_verbosity);
    }

private:
    HTKDeserializer* m_parent;
    ChunkIdType m_chunkId;
};

// Gets a data chunk with the specified chunk id.
ChunkPtr HTKDeserializer::GetChunk(ChunkIdType chunkId)
{
    return make_shared<HTKChunk>(this, chunkId);
};

// A matrix that stores all samples of a sequence without padding (differently from ssematrix).
// The number of columns equals the number of samples in the sequence.
// The number of rows equals the size of the feature vector of a sample (= dimensions).
class FeatureMatrix
{
public:
    FeatureMatrix(size_t numRows, size_t numColumns) : m_numRows(numRows), m_numColumns(numColumns)
    {
        m_data.resize(m_numRows * m_numColumns);
    }

    // Returns a reference to the column.
    inline array_ref<float> col(size_t column)
    {
        return array_ref<float>(m_data.data() + m_numRows * column, m_numRows);
    }

    // Gets pointer to the data.
    inline float* GetData()
    {
        return m_data.data();
    }

    // Gets the number of columns. It equals the number of samples in the sequence/utterance.
    inline size_t GetNumberOfColumns() const
    {
        return m_numColumns;
    }

    // Gets total size in elements of stored features.
    inline size_t GetTotalSize() const
    {
        return m_data.size();
    }

private:
    // Features
    std::vector<float> m_data;
    // Number of rows = dimension of the feature
    size_t m_numRows;
    // Number of columns = number of samples in utterance.
    size_t m_numColumns;
};

// This class stores sequence data for HTK for floats.
struct HTKFloatSequenceData : DenseSequenceData
{
    HTKFloatSequenceData(FeatureMatrix&& data) : m_buffer(data)
    {
        m_numberOfSamples = (uint32_t)data.GetNumberOfColumns();
        if (m_numberOfSamples != data.GetNumberOfColumns())
        {
            RuntimeError("Maximum number of samples per sequence exceeded.");
        }
    }

    const void* GetDataBuffer() override
    {
        return m_buffer.GetData();
    }

private:
    FeatureMatrix m_buffer;
};

// This class stores sequence data for HTK for doubles.
struct HTKDoubleSequenceData : DenseSequenceData
{
    HTKDoubleSequenceData(FeatureMatrix& data) : m_buffer(data.GetData(), data.GetData() + data.GetTotalSize())
    {
        m_numberOfSamples = (uint32_t)data.GetNumberOfColumns();
        if (m_numberOfSamples != data.GetNumberOfColumns())
        {
            RuntimeError("Maximum number of samples per sequence exceeded.");
        }
    }

    const void* GetDataBuffer() override
    {
        return m_buffer.data();
    }

private:
    std::vector<double> m_buffer;
};

// Copies a source into a destination with the specified destination offset.
static void CopyToOffset(const const_array_ref<float>& source, array_ref<float>& destination, size_t offset)
{
    size_t sourceSize = source.size() * sizeof(float);
    memcpy_s((char*)destination.begin() + sourceSize * offset, sourceSize, &source.front(), sourceSize);
}

// TODO: Move augmentation to the separate class outside of deserializer.
// TODO: Check the CNTK Book why different left and right extents are not supported.
// Augments a frame with a given index with frames to the left and right of it.
static void AugmentNeighbors(const MatrixAsVectorOfVectors& utterance,
                             size_t frameIndex,
                             const size_t leftExtent,
                             const size_t rightExtent,
                             array_ref<float>& destination)
{
    CopyToOffset(utterance[frameIndex], destination, leftExtent);

    for (size_t currentFrame = frameIndex, n = 1; n <= leftExtent; n++)
    {
        if (currentFrame > 0)
            currentFrame--; // index does not move beyond boundary
        CopyToOffset(utterance[currentFrame], destination, leftExtent - n);
    }

    for (size_t currentFrame = frameIndex, n = 1; n <= rightExtent; n++)
    {
        if (currentFrame + 1 < utterance.size())
            currentFrame++; // index does not move beyond boundary
        CopyToOffset(utterance[currentFrame], destination, leftExtent + n);
    }
}

// Get a sequence by its chunk id and sequence id.
// Sequence ids are guaranteed to be unique inside a chunk.
void HTKDeserializer::GetSequenceById(ChunkIdType chunkId, size_t id, vector<SequenceDataPtr>& r)
{
    const auto& chunkDescription = m_chunks[chunkId];
    size_t utteranceIndex = m_frameMode ? chunkDescription.GetUtteranceForChunkFrameIndex(id) : id;
    const UtteranceDescription* utterance = chunkDescription.GetUtterance(utteranceIndex);
    auto utteranceFrames = chunkDescription.GetUtteranceFrames(utteranceIndex);

    // wrapper that allows m[j].size() and m[j][i] as required by augmentneighbors()
    MatrixAsVectorOfVectors utteranceFramesWrapper(utteranceFrames);

    size_t utteranceLength = utterance->GetNumberOfFrames();
    if (m_frameMode)
    {
        // Always return a single frame only.
        utteranceLength = 1; 
    }
    else if (m_expandToPrimary)
    {
        if (r.empty())
            RuntimeError("Expansion of utterance is not allowed for primary deserializer.");

        // Getting the number of samples we have to extend to from the primary/first deserializer.
        utteranceLength = r.front()->m_numberOfSamples;
    }

    FeatureMatrix features(m_dimension, utteranceLength);
    if (m_frameMode)
    {
        // For frame mode augment a single frame.
        size_t frameIndex = id - chunkDescription.GetStartFrameIndexInsideChunk(utteranceIndex);
        auto fillIn = features.col(0);
        AugmentNeighbors(utteranceFramesWrapper, frameIndex, m_augmentationWindow.first, m_augmentationWindow.second, fillIn);
    }
    else
    {
        for (size_t resultingIndex = 0; resultingIndex < utteranceLength; ++resultingIndex)
        {
            auto fillIn = features.col(resultingIndex);
            AugmentNeighbors(utteranceFramesWrapper, m_expandToPrimary ? 0 : resultingIndex, m_augmentationWindow.first, m_augmentationWindow.second, fillIn);
        }
    }

    // Copy features to the sequence depending on the type.
    DenseSequenceDataPtr result;
    if (m_elementType == ElementType::tdouble)
        result = make_shared<HTKDoubleSequenceData>(features);
    else if (m_elementType == ElementType::tfloat)
        result = make_shared<HTKFloatSequenceData>(std::move(features));
    else
        LogicError("Currently, HTK Deserializer supports only double and float types.");

    result->m_key.m_sequence = utterance->GetId();
    r.push_back(result);
}

// Gets sequence description by its key.
bool HTKDeserializer::GetSequenceDescription(const SequenceDescription& primary, SequenceDescription& d)
{
    assert(!m_primary);
    auto found = std::lower_bound(m_keyToChunkLocation.begin(), m_keyToChunkLocation.end(), std::make_tuple(primary.m_key.m_sequence, 0, 0),
        [](const std::tuple<size_t, size_t, size_t>& a, const std::tuple<size_t, size_t, size_t>& b)
    {
        return std::get<0>(a) < std::get<0>(b);
    });

    if (found == m_keyToChunkLocation.end() || std::get<0>(*found) != primary.m_key.m_sequence)
    {
        return false;
    }

    auto chunkId = std::get<1>(*found);
    auto utteranceIndexInsideChunk = std::get<2>(*found);
    auto& chunk = m_chunks[chunkId];
    auto utterance = chunk.GetUtterance(utteranceIndexInsideChunk);

    d.m_chunkId = (ChunkIdType)chunkId;
    d.m_numberOfSamples = m_frameMode ? 1 : (uint32_t)utterance->GetNumberOfFrames();

    if (m_frameMode && !m_expandToPrimary)
    {
        d.m_indexInChunk = chunk.GetStartFrameIndexInsideChunk(utteranceIndexInsideChunk) + primary.m_key.m_sample;

        // Check that the sequences are equal in number of frames.
        if (primary.m_key.m_sample >= utterance->GetNumberOfFrames())
            RuntimeError("Sequence with key '%s' has '%zu' frame(s), whereas the primary sequence expects at least '%d' frames",
                m_corpus->IdToKey(primary.m_key.m_sequence).c_str(), utterance->GetNumberOfFrames(), primary.m_key.m_sample + 1);
    }
    else
    {
        d.m_indexInChunk = utteranceIndexInsideChunk;
    }

    return true;
}

}}}
