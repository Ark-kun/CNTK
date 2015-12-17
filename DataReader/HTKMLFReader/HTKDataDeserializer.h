#pragma once

#include "InnerInterfaces.h"
#include "BundlerSplitted.h"

namespace Microsoft { namespace MSR { namespace CNTK {

    struct chunkdata       // data for a chunk of utterances
    {
        std::vector<utterancedesc*> utteranceset;    // utterances in this set
        size_t numutterances() const { return utteranceset.size(); }

        std::vector<size_t> firstframes;    // [utteranceindex] first frame for given utterance
        mutable msra::dbn::matrix frames;   // stores all frames consecutively (mutable since this is a cache)
        size_t totalframes;         // total #frames for all utterances in this chunk
        mutable std::vector<shared_ptr<const msra::dbn::latticesource::latticepair>> lattices;   // (may be empty if none)

        // construction
        chunkdata() : totalframes(0) {}
        void push_back(utterancedesc* utt)
        {
            if (isinram())
                LogicError("utterancechunkdata: frames already paged into RAM--too late to add data");
            firstframes.push_back(totalframes);
            totalframes += utt->numframes();
            utteranceset.push_back(utt);
        }

        // accessors to an utterance's data
        size_t numframes(size_t i) const { return utteranceset[i]->numframes(); }
        size_t getclassidsbegin(size_t i) const { return utteranceset[i]->classidsbegin; }
        msra::dbn::matrixstripe getutteranceframes(size_t i) const // return the frame set for a given utterance
        {
            if (!isinram())
                LogicError("getutteranceframes: called when data have not been paged in");
            const size_t ts = firstframes[i];
            const size_t n = numframes(i);
            return msra::dbn::matrixstripe(frames, ts, n);
        }
        shared_ptr<const msra::dbn::latticesource::latticepair> getutterancelattice(size_t i) const // return the frame set for a given utterance
        {
            if (!isinram())
                LogicError("getutterancelattice: called when data have not been paged in");
            return lattices[i];
        }

        // paging
        // test if data is in memory at the moment
        bool isinram() const { return !frames.empty(); }
        // page in data for this chunk
        // We pass in the feature info variables by ref which will be filled lazily upon first read
        void requiredata(string & featkind, size_t & featdim, unsigned int & sampperiod, const msra::dbn::latticesource & latticesource, int verbosity = 0) const
        {
            if (numutterances() == 0)
                LogicError("requiredata: cannot page in virgin block");
            if (isinram())
                LogicError("requiredata: called when data is already in memory");
            try             // this function supports retrying since we read from the unrealible network, i.e. do not return in a broken state
            {
                msra::asr::htkfeatreader reader;    // feature reader (we reinstantiate it for each block, i.e. we reopen the file actually)
                // if this is the first feature read ever, we explicitly open the first file to get the information such as feature dimension
                if (featdim == 0)
                {
                    reader.getinfo(utteranceset[0]->parsedpath, featkind, featdim, sampperiod);
                    fprintf(stderr, "requiredata: determined feature kind as %llu-dimensional '%s' with frame shift %.1f ms\n",
                        featdim, featkind.c_str(), sampperiod / 1e4);
                }
                // read all utterances; if they are in the same archive, htkfeatreader will be efficient in not closing the file
                frames.resize(featdim, totalframes);
                if (!latticesource.empty())
                    lattices.resize(utteranceset.size());
                foreach_index(i, utteranceset)
                {
                    //fprintf (stderr, ".");
                    // read features for this file
                    auto uttframes = getutteranceframes(i);    // matrix stripe for this utterance (currently unfilled)
                    reader.read(utteranceset[i]->parsedpath, (const string &)featkind, sampperiod, uttframes);  // note: file info here used for checkuing only
                    // page in lattice data
                    if (!latticesource.empty())
                        latticesource.getlattices(utteranceset[i]->key(), lattices[i], uttframes.cols());
                }
                //fprintf (stderr, "\n");
                if (verbosity)
                    fprintf(stderr, "requiredata: %d utterances read\n", (int)utteranceset.size());
            }
            catch (...)
            {
                releasedata();
                throw;
            }
        }
        // page out data for this chunk
        void releasedata() const
        {
            if (numutterances() == 0)
                LogicError("releasedata: cannot page out virgin block");
            if (!isinram())
                LogicError("releasedata: called when data is not memory");
            // release frames
            frames.resize(0, 0);
            // release lattice data
            lattices.clear();
        }
    };

    struct Utterance : public SequenceDescription
    {
        Utterance(utterancedesc&& u) : utterance(u) {}

        utterancedesc utterance;
        size_t indexInsideChunk;
    };

    // Should not this be splitted to different deserializers?
    struct Frame : public SequenceDescription
    {
        Frame(Utterance* u) : u(u) {}

        Utterance* u;
        size_t frameIndexInUtterance;
    };

    class HTKDataDeserializer : public DataDeserializer
    {
        size_t m_dimension;
        SampleLayoutPtr m_layout;
        std::vector<std::wstring> m_featureFiles;

        std::vector<Utterance> m_utterances;
        std::vector<Frame> m_frames;

        size_t m_elementSize;
        TimelineP m_sequences;

        std::vector<chunkdata> m_chunks;
        size_t m_chunksinram;             // (for diagnostics messages)

        size_t m_featdim;
        unsigned int m_sampperiod;        // (for reference and to check against model)
        int m_verbosity;
        std::string m_featKind;
        std::pair<size_t, size_t> m_context;
        bool m_frameMode;
        std::wstring m_featureName;

    public:
        HTKDataDeserializer(const ConfigParameters& feature, size_t elementSize, bool frameMode, const std::wstring& featureName);

        virtual void SetEpochConfiguration(const EpochConfiguration& config) override;

        virtual const TimelineP& GetSequenceDescriptions() const override;

        virtual std::vector<InputDescriptionPtr> GetInputs() const override;

        virtual std::vector<Sequence> GetSequenceById(size_t id) override;

        virtual bool RequireChunk(size_t chunkIndex) override;

        virtual void ReleaseChunk(size_t chunkIndex) override;

    public:
        const std::vector<Utterance>& GetUtterances() const;
    };

    typedef std::shared_ptr<HTKDataDeserializer> HTKDataDeserializerPtr;
}}}