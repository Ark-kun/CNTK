//
// <copyright file="ComputationNode.cpp" company="Microsoft">
//     Copyright (c) Microsoft Corporation.  All rights reserved.
// </copyright>
//

#define _CRT_SECURE_NO_WARNINGS // "secure" CRT not available on all platforms  --add this at the top of all CPP files that give "function or variable may be unsafe" warnings

#include "ComputationNode.h"
#include "InputAndParamNodes.h"
#include "ComputationNetworkBuilder.h" // TODO: We should only pull in NewComputationNodeFromConfig(). Nodes should not know about network at large.
#include "TensorShape.h"

#ifndef let
#define let const auto
#endif

namespace Microsoft { namespace MSR { namespace CNTK {

using namespace std;

// -----------------------------------------------------------------------
// subroutines for Validate() implementations
// -----------------------------------------------------------------------

// helper function to infer the MBLayout for this node from inputs, for the *standard case*
// the standard case is:
//  - all inputs must share the same layout (e.g. adding two minibatches)
//  - with the exception of NULL layouts (e.g. TimesNode)
//  - all layouts may be NULL (e.g. W' = W * Exp(Stabilizer))
//  - if there are more than one different layouts involved, this function will fail
void ComputationNodeBase::InferMBLayoutFromInputsForStandardCase()
{
    MBLayoutPtr pMBLayout; // start with NULL layout
    for (auto child : m_inputs)
    {
        if (!child) // node not set yet (DelayedValueNodeBase seems to allow this)--BUGBUG: Then this function won't operate correctly.
            ;
        else if (!child->m_pMBLayout) // NULL layout (typical for parameter nodes)
            ;
        else if (!pMBLayout) // first non-NULL layout: just copy it
            pMBLayout = child->m_pMBLayout;
        else if (pMBLayout != child->m_pMBLayout) // got a layout--compare whether it is the same
            RuntimeError("InferMBLayoutFromInputsForStandardCase: Found inconsistent layout in %ls %ls operation, mismatch detected for child %ls %ls.",
                         NodeName().c_str(), OperationName().c_str(), child->NodeName().c_str(), child->OperationName().c_str());
    }
    // all are consistent: install it
    LinkToMBLayout(pMBLayout);
}

// single input that maps its input element-wise (e.g. Sigmoid)
void ComputationNodeBase::ValidateUnaryMap(bool isFinalValidationPass)
{
    assert(m_inputs.size() == 1);
    ComputationNodeBase::Validate(isFinalValidationPass);
    InferMBLayoutFromInputsForStandardCase();
    SetDims(Input(0));
}
// binary zip operation, e.g. Plus
// If allowScaling then one can be a sub-dimension of the other (if layout then only for rows, otherwise for cols, too).
// This also helpfully resizes the children if not yet sized.
void ComputationNodeBase::ValidateBinaryZip(bool isFinalValidationPass, bool allowMultiples)
{
    assert(m_inputs.size() == 2);
    ComputationNodeBase::Validate(isFinalValidationPass);
    InferMBLayoutFromInputsForStandardCase();

    ValidateInferBinaryInputDims();

    size_t rows0 = Input(0)->GetNumRows(), cols0 = Input(0)->GetNumCols();
    size_t rows1 = Input(1)->GetNumRows(), cols1 = Input(1)->GetNumCols();

#if 1 //ndef ENABLE_TENSORVIEW
    // TODO: This test will go away once we switch to full tensor lib.
    if (isFinalValidationPass && !((rows0 == rows1 && (Input(0)->GetMBLayout() == Input(1)->GetMBLayout() || cols0 == cols1)) ||                                                               // matching size (obvious case)
                                   (allowMultiples && (rows0 == 1 || rows1 == 1) && (Input(0)->GetMBLayout() == Input(1)->GetMBLayout() || cols0 == cols1)) ||                                 // one is row vec
                                   (allowMultiples && ((!HasMBLayout() && cols0 > cols1 && cols0 % cols1 == 0) || (cols0 == 1 && rows1 % rows0 == 0) || (cols1 == 1 && rows0 % rows1 == 0))))) // TODO: ^^ I don't understand the asymmetry of this last one
    {
        LogicError("The Matrix dimensions in the %ls %ls operation do not match.", NodeName().c_str(), OperationName().c_str());
    }
#else
    rows0;
    rows1;
#endif

    // result has tensor shape with dimensions being the max over both
    let shape0 = GetInputSampleLayout(0);
    let shape1 = GetInputSampleLayout(1);
    SmallVector<size_t> dims = shape0.GetDims();
    if (shape1.GetRank() > dims.size())
        dims.resize(shape1.GetRank(), 1); // pad with ones

    // If rank of [0] is higher than we only need to take max over rank [1].
    // If rank of [1] is higher then we have padded to equal lentgh.
    for (size_t k = 0; k < shape1.GetRank(); k++)
    {
        size_t dim1 = shape1[k];
        if (dims[k] == 1)                                  // is [0] broadcasting?
            dims[k] = dim1;                                // then use dimension we broadcast to
        else if (dim1 == 1)                                // if [1] is broadcasting
            ;                                              // dims is already correct
        else if (isFinalValidationPass && dim1 != dims[k]) // no broadcasting: they must match
            InvalidArgument("%ls %ls operation: Input dimensions [%s] and [%s] are not compatible.",
                            NodeName().c_str(), OperationName().c_str(), string(shape0).c_str(), string(shape1).c_str());
    }

    SetDims(TensorShape(dims), GetMBLayout() ? GetMBLayout()->GetNumCols() : max(cols0, cols1));
}
// unary reduce-to-(1,1) operation, e.g. MatrixL1RegNode
void ComputationNodeBase::ValidateUnaryReduce(bool isFinalValidationPass)
{
    assert(m_inputs.size() == 1);
    ComputationNodeBase::Validate(isFinalValidationPass);
    m_pMBLayout = nullptr; // this node does not hold mini-batch data
    SetDims(TensorShape(1), 1);
}
// binary reduce-to-(1,1) operation, e.g. CrossEntropyWithSoftmaxNode
// Currently only called by criterion nodes.
// This function also infers child LearnableParameters. In case you wonder why this is needed for criterion nodes, there are edge cases, e.g. a
// learnable parameter being regularized by a criterion node, where the learnable parameter is fed both into that criterion node and other places.
void ComputationNodeBase::ValidateBinaryReduce(bool isFinalValidationPass)
{
    ComputationNodeBase::Validate(isFinalValidationPass);
    m_pMBLayout = nullptr; // this node does not hold mini-batch data
    ValidateInferBinaryInputDims();
    if (isFinalValidationPass &&
        !(Input(0)->GetNumRows() == Input(1)->GetNumRows() &&
          (Input(0)->HasMBLayout() || (Input(0)->GetNumCols() == Input(1)->GetNumCols()))))
        LogicError("The Matrix dimensions in the %ls %ls operation do not match.", NodeName().c_str(), OperationName().c_str());
    SetDims(TensorShape(1), 1);
}
// helper function for validation
// In bad cases of convolution, dimensions are quite complex to know.
// This is a feature that allows a node to help resizing its input node to the expected value.
// TODO: This is shaky by design.
void ComputationNodeBase::ValidateInferBinaryInputDims()
{
    // limited inference of children dimensions
    // if dimension not specified we assume two operands' dimensions should be the same
    // NOTE: The assert is set to check if >= 2 since this is called from nodes which have more than two children.
    //      The number of children is formally verified elsewhere, so this will not break consistency.
    assert(m_inputs.size() >= 2);
    for (size_t index = 0; index < 2; index++)
    {
        auto in = Input(index);
        auto other = Input(1 - index);
        // borrow any unset dimension on one input from the other input
        size_t rows = in->GetNumRows() == 0 ? other->GetNumRows() /*borrow from peer*/ : in->GetNumRows() /*keep as is*/;
        size_t cols = (!in->HasMBLayout() && in->GetNumCols() == 0) ? other->GetNumCols() /*borrow from peer*/ : in->GetNumCols() /*keep as is*/;
        ValidateInferInputDims(index, rows, cols);
    }
}
// BUGBUG: Change this to take a TensorShape.
template <class ElemType>
void ComputationNode<ElemType>::ValidateInferInputDims(size_t i, size_t rows, size_t cols) //override final
{
    if (Input(i)->OperationName() == OperationNameOf(LearnableParameter) && Input(i)->GetNumRows() == 0)
    {
        if (rows == 0 || cols == 0)
            LogicError("ValidateInferInputDims: Inferred matrix must not be empty.");
        Input(i)->SetDims(rows == Input(i)->GetNumRows() ? Input(i)->GetSampleLayout() : TensorShape(rows), cols);
        // BUGBUG: This will loose tensor shape.
        Input(i)->Validate(true); // validate it properly
        // BUGBUG: ^^ Validate() calls are under the control of ValidateSubNetwork(). E.g. it checks whether something has changed & re-validates until there is no change. If we validate here, the change goes unnoticed.
        // big BUGBUG: This should do random initialization as requested by user in the first place.
        Input(i)->Value().SetValue(0);
        fprintf(stderr, "ValidateInferInputDims: %ls %ls operation inferred, resized to (%d x %d), and (incorrectly) initialized to 0.\n", Input(i)->NodeName().c_str(), Input(i)->OperationName().c_str(), (int) rows, (int) cols);
    }
}

// -----------------------------------------------------------------------
// tensor helpers
// -----------------------------------------------------------------------

// determine the sample tensor dimension to use for operations based on output and all inputs
// 'Sample tensor' means we only consider single samples. If we have an MBLayout, that is the sample layout of a single matrix column.
size_t ComputationNodeBase::DetermineElementwiseTensorRank() const
{
    // determine largest tensor dimension amongst the sample shapes of output and the selected inputs
    size_t maxRank = GetSampleLayout().GetRank();
    for (size_t i = 0; i < GetNumInputs(); i++)
    {
        size_t rank = Input(i)->GetSampleLayout().GetRank();
        if (!HasMBLayout()) // no MBLayout: last dim is column dimension
            rank++;
        if (maxRank < rank)
            maxRank = rank;
    }
    return maxRank;
}

// form the actual tensor that describes the full object
TensorShape ComputationNodeBase::GetTensorShape(size_t rank) const
{
    // If we have an MB layout then add the necessary dimensions. If we have none, then absorb the column dimension.
    TensorShape tensorShape = GetSampleLayout(); // TODO: Can this tensor arbitrary strides? In case it came out of a Slice, Reshape, or Transpose op in-place
    if (!HasMBLayout())
        tensorShape.AppendInPlace(tensorShape.GetRank(), GetNumCols()); //  last dim is column dimension
    // TODO: This is not nice! Instead, if no MBLayout then have sample layout explain whole matrix.
    else
        tensorShape.AppendInPlace(rank, GetMBLayout()->GetNumParallelSequences()).AppendInPlace(rank + 1, GetMBLayout()->GetNumTimeSteps());
    return tensorShape;
}

// get tensor shape of the slice referenced by a given FrameRange
TensorShape ComputationNodeBase::GetTensorSliceFor(size_t rank, const FrameRange& fr) const
{
    // form the actual tensor that describes the full object
    // Note: This may have strides.
    auto tensorShape = GetTensorShape(rank);

    // determine the slice dimensions described by the FrameRange
    // Note: These are dimensions without strides.
    auto slice = TensorSliceWithMBLayoutFor(tensorShape.GetDims(), fr, GetMBLayout());

    // narrow the tensor
    // Note: Strides are honored correctly.
    tensorShape.NarrowTo(slice);
    return tensorShape;
}

// -----------------------------------------------------------------------
// others
// -----------------------------------------------------------------------

template <class ElemType>
/*virtual*/ void ComputationNode<ElemType>::DumpNodeInfo(const bool /*printValues*/, File& fstream) const
{
    fstream << L"\n" + NodeName() + L"=" + OperationName();

    if (!IsLeaf())
    {
        fstream << wstring(L"(");
        for (size_t i = 0; i < GetNumInputs(); i++)
        {
            if (i > 0)
                fstream << wstring(L",");
            fstream << (Input(i) ? Input(i)->NodeName() : L"NULL");
        }
        fstream << wstring(L")");
    }
}

// -----------------------------------------------------------------------
// instantiate the core class templates
// -----------------------------------------------------------------------

typedef Matrix<float> FloatMatrix;
typedef Matrix<double> DoubleMatrix;

atomic_ullong TimeStamp::s_timeStampCounter = ATOMIC_VAR_INIT(0);

template <>
std::map<size_t, std::map<size_t, FloatMatrix*>> ComputationNode<float>::s_constOnes{};
template <>
std::map<size_t, std::map<size_t, DoubleMatrix*>> ComputationNode<double>::s_constOnes{};

template class ComputationNode<float>;
template class ComputationNode<double>;

template class LearnableParameter<float>;
template class LearnableParameter<double>;
}
}
}

namespace Microsoft { namespace MSR { namespace ScriptableObjects {

using namespace Microsoft::MSR::CNTK;

// -----------------------------------------------------------------------
// register ComputationNode with the ScriptableObject system
// -----------------------------------------------------------------------

template <>
shared_ptr<Object> MakeRuntimeObject<ComputationNodeBase>(const IConfigRecordPtr configp)
{
    return NewComputationNodeFromConfig(configp);
}

ScriptableObjects::ConfigurableRuntimeTypeRegister::Add<ComputationNodeBase> registerComputationNode(L"ComputationNode");

// -----------------------------------------------------------------------
// register a boxed version of TensorShape with the ScriptableObject system
// -----------------------------------------------------------------------

// e.g.
// new TensorShape [ dims = 13:42 ]
class BoxedTensorShape : public BoxOf<TensorShape>
{
    // create a TensorShape from config
    static TensorShape TensorShapeFromConfig(const IConfigRecord& config)
    {
        const auto& valp = config[L"dims"];
        if (valp.Is<TensorShape>())
            return valp.AsRef<TensorShape>(); // UNTESTED
        else if (valp.Is<ConfigArray>())
            return TensorShape(valp.AsRef<ConfigArray>().AsVector<size_t>([&](const wstring& msg)
                                                                          {
                                                                              valp.Fail(msg);
                                                                          }));
        else
            return TensorShape(std::vector<size_t>(1, (size_t) valp)); // single element
    }

public:
    BoxedTensorShape(const IConfigRecordPtr configp)
        : BoxOf<TensorShape>(TensorShapeFromConfig(*configp))
    {
    }
};

template <typename E>
class BoxedVector : public BoxOf<vector<E>>
{
    // create a vector from config
    static vector<E> VectorFromConfig(const IConfigRecord& config)
    {
        const auto& valp = config[L"items"];
        if (valp.Is<vector<E>>())
            return valp.AsRef<vector<E>>(); // UNTESTED
        else if (valp.Is<ConfigArray>())
            return valp.AsRef<ConfigArray>().AsVector<E>([&](const wstring& msg)
                                                         {
                                                             valp.Fail(msg);
                                                         });
        else
            return std::vector<E>(1, (E) valp); // single element
    }

public:
    BoxedVector(const IConfigRecordPtr configp)
        : BoxOf<vector<E>>(VectorFromConfig(*configp))
    {
    }
};

ScriptableObjects::ConfigurableRuntimeTypeRegister::Add<BoxedTensorShape> registerTensorShape(L"TensorShape");
ScriptableObjects::ConfigurableRuntimeTypeRegister::Add<BoxedVector<int>> registerIntVector(L"IntVector");
ScriptableObjects::ConfigurableRuntimeTypeRegister::Add<BoxedVector<size_t>> registerSizeVector(L"SizeVector");
} } }
