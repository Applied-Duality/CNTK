//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//

#include "CNTKToONNX.h"
#include "proto/onnx/core/model.h"
#include "proto/onnx/core/graph.h"
#include "proto/onnx/core/status.h"

#include "Utils.h"
#include "Operators.h"
#include "BlockFunction.h"
#include <vector>
#include <tuple>
#include <numeric>

#include "Matrix.h"
//#include "CPUSparseMatrix.h"
//#include "GPUSparseMatrix.h"

using namespace Microsoft::MSR::CNTK;

using namespace CNTK::ONNX;
using namespace CNTK;

onnx::TypeProto TensorShapeProtoToTypeProto(const onnx::TensorShapeProto* inputShape)
{
    onnx::TypeProto newShape;
    int inputRank = inputShape->dim_size();
    for (int index = 0; index < inputRank; index++)
        newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(inputShape->dim(index).dim_value());

    return newShape;
}

bool HasSequenceAxis(Variable operand)
{
    return (operand.DynamicAxes().size() - (operand.HasBatchAxis() ? 1 : 0)) > 0;
}

//
// Helper function to reduce the rank of a shape.
//
onnx::TypeProto ReduceRank(const onnx::TensorShapeProto* inputShape, int reductionRank, bool rightReduction)
{
    assert(inputShape != nullptr);

    int inputRank = inputShape->dim_size();
    assert(inputRank > reductionRank);

    onnx::TypeProto newShape;
    int64_t reduceDim = 1;

    if (rightReduction)
    {
        for (int index = 0; index < (inputRank - reductionRank); index++)
            newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(inputShape->dim(index).dim_value());

        for (int index = (inputRank - reductionRank); index < inputRank; index++)
            reduceDim *= inputShape->dim(index).dim_value();

        newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(reduceDim);
    }
    else
    {
        for (int index = 0; index < reductionRank; index++)
            reduceDim *= inputShape->dim(index).dim_value();

        newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(reduceDim);

        for (int index = reductionRank; index < inputRank; index++)
            newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(inputShape->dim(index).dim_value());
    }

    return newShape;
}

namespace CNTK
{

    class CNTKToONNXHelper
    {
    public:
        //
        // Copy the entire CNTK graph to ONNX graph.
        //
        static void Copy(const FunctionPtr& src, ONNXIR::Graph* dst);

    private:
        //
        // Recursively create ONNX nodes corresponding to each CNTK node.
        //
        static ONNXIR::Node* CreateNode(const FunctionPtr& src,
            ONNXIR::Graph* graph,
            std::unordered_map<FunctionPtr, ONNXIR::Node*>& functionNodes,
            std::unordered_map<Variable, ONNXIR::Node*>& variableNodes,
            const std::unordered_map<Variable, Variable>& compositeOutputsMap);

        //
        // Traverse the entire graph and collect variable mapping between graph inside and outside the block.
        //
        static void TraverseGraph(const FunctionPtr& src,
            std::set<FunctionPtr>& visited,
            std::unordered_map<Variable, Variable>& compositeOutputsMap);

        // Remove it after testing.
        static void CNTKToONNXHelper::PrintNDArrayView(const NDArrayViewPtr src);

        //
        // Copy the content of NDArrayView to TensorProto, and do the needed
        // convergence.
        //
        static void CopyTensor(const NDArrayViewPtr src, onnx::TensorProto& dst, onnx::TypeProto *inputArgType = nullptr);

        //
        // Copy supported attributes from CNTK node to corresponding ONNX node.
        //
        static void CopyAttributes(const FunctionPtr& src, ONNXIR::Node* node);

        //
        // Convert Axis object to actual tensor index.
        //
        static int ToIndex(const Axis& axis);

        //
        // Convert NDShape and various std::vector types to TensorShape
        //
        static onnx::TypeProto ToTypeProto(const NDShape& shape, bool hasBatchAxis = false, bool hasSequenceAxis = false, bool doReverseShape = true);
        static onnx::TypeProto ToTypeProto(const std::vector<bool>& shape);
        static onnx::TypeProto ToTypeProto(const std::vector<int>& shape, bool doReverseVec = true);
        static onnx::TypeProto ToTypeProto(const std::vector<Axis>& axes);

        //
        // Convert TypeProto, NDShape and various std::vector types to std::vector
        //
        static std::vector<int64_t> ToINTS(const onnx::TypeProto& shape);
        static std::vector<int64_t> ToINTS(const NDShape& shape, bool hasBatchAxis = false);
        static std::vector<int64_t> ToINTS(const std::vector<bool>& shape);
        static std::vector<int64_t> ToINTS(const std::vector<int>& shape, bool doReverseVec = true);
        static std::vector<int64_t> ToINTS(const std::vector<Axis>& axes);

        static std::vector<float> INTSToVecFloat(const std::vector<int64_t> &ints);
        static std::vector<int64_t> ConvertPermutationCNTKToONNX(const std::vector<Axis> &axes, bool hasBatchAxis);

        //
        // Convert data types from CNTK to ONNX.
        //
        static void UpdateONNXType(DataType dataType, onnx::TypeProto& type);

        //
        // Map CNTK OP names to ONNX OP Names.
        //
        static std::string ToOPName(const FunctionPtr& src);

        static bool OpInputsHasBatchAxis(const FunctionPtr& src);

        //
        // Which input to ignore during converting a CNTK block to a primitive OP in ONNX.
        //
        static bool FilterInput(const FunctionPtr& src, const CNTK::Variable& input, size_t inputIndex);

        //
        // Converts axis (in CNTK C++ API sense) to index in ONNX sense
        //
        static int64_t ConvertAxisToOnnx(const Axis &axis, const Variable &operand);

        //
        // Converts axes (in CNTK C++ API sense) to index in ONNX sense
        //
        static std::vector<int64_t> ConvertAxesToOnnx(const std::vector<Axis> &axes, const Variable &operand);

        //
        // Given input tersors of a CNTK elementwise operation, figure out
        // input shapes for ONNX operation.
        // It also returns whether broadcast is required and the axis for broadcast.
        // Due to the fact that ONNX only allows braodcast of right-hand-side,
        // inputs may need to be swapped. In this case the last bool is true.
        static std::tuple<std::pair<std::vector<int>, std::vector<int>>, bool, int, bool> AdjustForBroadcastShape(
            const Variable &input1, const Variable &input2);

        static std::tuple<std::vector<int>, bool, int, bool > CalculateBroadcastAxis(
            const std::vector<int> &dims1, const std::vector<int> &dims2);

        //
        // Argument orders between CNTK and ONNX aren't always the same.
        //
        static std::vector<ONNXIR::NodeArg> MapInputsOrderToONNX(const FunctionPtr& src, const std::vector<ONNXIR::NodeArg>& inputs);

        //
        // Add current CNTK node to ONNX graph.
        //
        static ONNXIR::Node* AddNode(const FunctionPtr& src, ONNXIR::Graph* graph, const std::vector<ONNXIR::NodeArg>& inputs, const std::vector<ONNXIR::NodeArg>& outputs);

        //
        // Get ONNX 'pads' attribute value based on CNTK node's autoPadding attribute value.
        //
        static std::pair<std::vector<int>, std::vector<int> > GetONNXPadsAttributeFromCNTKNode(
            const std::vector<bool>& cntkAutoPadding, const NDShape& kernelShape, bool ceilOutDim);

        //
        // Adds attributes 'auto_pad' or 'pads' to saved node (typically convolution or pooling).
        //
        static void PutAutopadOrPadAttrInNode(ONNXIR::Node* node, const std::vector<bool>& autoPadding,
            const NDShape& kernelShape, bool ceilOutDim = false);

        //
        // Takes CNTK's OptimizedRNNStack node and converts it into a series of RNN/LSTM/GRU nodes
        // on the ONNX side.
        //
        static ONNXIR::Node* CreateONNXNodesForOptimizedRNNStack(const FunctionPtr &src,
            ONNXIR::Graph* graph,
            std::unordered_map<FunctionPtr, ONNXIR::Node*>& functionNodes,
            std::unordered_map<Variable, ONNXIR::Node*>& variableNodes,
            const std::unordered_map<Variable, Variable>& compositeOutputsMap);

        //
        // Takes the OptimizedRNNStack's input combined weight matrix, and splits it into individual
        // weight and bias matrices for each recurrent op layer.
        //
        static std::tuple<std::vector<NDArrayViewPtr>, std::vector<NDArrayViewPtr>, std::vector<NDArrayViewPtr> >
            SplitOptimzedRnnWtoIndivMats(Matrix<float>& WbigIn, size_t numLayers, size_t inputSize, size_t hiddenSize,
                bool bidirectional, wstring recurrentOp = L"lstm");

        //
        // Extracts RNN weight matrices from OptimizedRNNStack's input combined weight matrix.
        //
        static Matrix<float> GetWeightMatFromOrnnBigW(Matrix<float>& Wbig, size_t offset,
            size_t layerInputSize, size_t layerOutputSize, size_t numGates, wstring recurrentOp = L"lstm");

        //
        // Extracts RNN bias matrices from OptimizedRNNStack's input combined weight matrix.
        //
        static Matrix<float> CNTKToONNXHelper::GetBiasMatFromOrnnBigW(Matrix<float>& Wbig, size_t offset,
            size_t hiddenSize, size_t numGates, wstring recurrentOp = L"lstm");

        //
        // Takes the OptimizedRNNStack's individual weight matrix and changes the format from 
        // i,f,c,o (OptimizedRNNStack) to i,o,f,c (ONNX).
        //
        static void InplaceAdjustGateOrder(Matrix<float>& Wbig, size_t hiddenSize);

        //
        // Takes a vector of Matrix<ElemType> which are weights for each layer and each direction
        // and converts them to a vector of NDArrays, one for each layer, in ONNX LSTM format.
        //
        static std::vector<NDArrayViewPtr> ToRnnWeightPerLayerOnnxFormat(std::vector<Matrix<float> >& W, size_t numLayers,
            size_t numDirections, size_t numGates, size_t hiddenSize, size_t inputSize, bool updateInputSizeWithEachLayer);

        //
        // Takes a vector of Matrix<ElemType> which are biases for each layer and each direction
        // and converts them to a vector of NDArrays, one for each layer, in ONNX LSTM format.
        //
        static std::vector<NDArrayViewPtr> ToRnnBiasPerLayerOnnxFormat(std::vector<Matrix<float> >& W, size_t numLayers,
            size_t numDirections, size_t hiddenSize, size_t numGates);

        //
        // Create a ONNX node for input weight for a recurrence node.
        //
        static void CreateRecurrentWeightONNXNodes(ONNXIR::Graph* graph, std::unordered_map<Variable, ONNXIR::Node*>& variableNodes,
            const Variable& Wcombined, std::vector<ONNXIR::NodeArg>& inputs, NDArrayViewPtr W, string WArgName = "");

        //
        // Method to insert reshape and transpose nodes to the output of the ONNX LSTM output
        // so that it can be fed in as input to the next ONNX LSTM node.
        //
        static ONNXIR::NodeArg LSTMOutputShapeAdapter(ONNXIR::NodeArg& inputArg, onnx::TypeProto& inputArgType, ONNXIR::Graph* graph,
            size_t numDirections, size_t hiddenSize, CNTK::DataType outputType, string adapterBasename = "");

        // A helper function, to reverse any iterable container and return a copy
        // of the reversed container.
        //
        template<typename ItrType>
        static ItrType reverse(ItrType v)
        {
            std::reverse(std::begin(v), std::end(v));
            return v;
        }

        template<class T, class V>
        static inline std::vector<V> Cast(const std::vector<T>& v)
        {
            std::vector<V> result;
            result.reserve(v.size());
            for (auto d : v)
                result.push_back((V)d);
            return result;
        }
    };
}

std::unique_ptr<ONNXIR::Model> CNTKToONNX::CreateModel(const FunctionPtr& src)
{
    std::unique_ptr<ONNXIR::Model> model(new ONNXIR::Model("CNTKGraph", true));
    auto dstGraph = model->MainGraph();
    CNTKToONNXHelper::Copy(src, dstGraph);
    ONNXIR::Common::Status status = dstGraph->Resolve();
    if (!status.Ok())
        LogicError("%s", status.ErrorMessage().c_str());
    model->SetModelversion(static_cast<ONNXIR::VERSION>(CNTK_ONNX_MODEL_VERSION)); // This is the default. Should be surfaced as graph's 'save' API input.
    model->SetProducerVersion(CNTK_ONNX_PRODUCER_VERSION);
    model->SetProducerName(CNTK_ONNX_PRODUCER_NAME);
    return model;
}

void CNTKToONNXHelper::Copy(const FunctionPtr& src, ONNXIR::Graph* dst)
{
    std::set<FunctionPtr> visited;
    std::unordered_map<Variable, Variable> compositeOutputsMap;
    std::unordered_map<FunctionPtr, ONNXIR::Node*> functionNodes;
    std::unordered_map<Variable, ONNXIR::Node*> variableNodes;

    //
    // Traverse the graph and collect some information.
    //
    TraverseGraph(src, visited, compositeOutputsMap);

    //
    // Iterate through each node in CNTK graph and create an equivalent node
    // in ONNX graph.
    //
    CreateNode(src, dst, functionNodes, variableNodes, compositeOutputsMap);
}

void CNTKToONNXHelper::PrintNDArrayView(const NDArrayViewPtr src)
{
    auto dataType = src->GetDataType();
    auto srcTemp = src->DeepClone();
    auto srcShape = srcTemp->Shape();
    auto totalSize = srcShape.TotalSize();

    // This is our own copy so move it to the CPU.
    srcTemp->ChangeDevice(DeviceDescriptor::CPUDevice());

    switch (dataType)
    {
    case DataType::Float:
    {
        auto data = srcTemp->DataBuffer<float>();
        for (size_t index = 0; index < totalSize; index++)
            printf("%f", data[index]);
        break;
    }
    case DataType::Double:
    {
        auto data = srcTemp->DataBuffer<double>();
        for (size_t index = 0; index < totalSize; index++)
            printf("%f", data[index]);
        break;
    }
    default:
        NOT_IMPLEMENTED;
    }
}

void CNTKToONNXHelper::CopyTensor(const NDArrayViewPtr src, onnx::TensorProto& dst, onnx::TypeProto *inputArgType /*=nullptr*/)
{
    auto dataType = src->GetDataType();
    auto srcTemp = src->DeepClone();
    auto srcShape = srcTemp->Shape();
    auto totalSize = srcShape.TotalSize();

    // This is our own copy so move it to the CPU.
    srcTemp->ChangeDevice(DeviceDescriptor::CPUDevice());

    switch (dataType)
    {
    case DataType::Float:
    {
        dst.set_data_type(onnx::TensorProto_DataType_FLOAT);
        auto data = srcTemp->DataBuffer<float>();
        for (size_t index = 0; index < totalSize; index++)
            *(dst.mutable_float_data()->Add()) = data[index];

        break;
    }
    case DataType::Double:
    {
        dst.set_data_type(onnx::TensorProto_DataType_DOUBLE);
        auto data = srcTemp->DataBuffer<double>();
        for (size_t index = 0; index < totalSize; index++)
            *(dst.mutable_double_data()->Add()) = data[index];

        break;
    }
    default:
        NOT_IMPLEMENTED;
    }

    // use 
    if (inputArgType != nullptr)
    {
        std::vector<int64_t> dimensions = CNTKToONNXHelper::ToINTS(*inputArgType);
        for (auto dim : dimensions)
            *(dst.mutable_dims()->Add()) = dim;
    }
    else
    {
        auto dimensions = CNTKToONNXHelper::reverse(srcShape.Dimensions());
        for (auto dim : dimensions)
            *(dst.mutable_dims()->Add()) = dim;
    }
}

int CNTKToONNXHelper::ToIndex(const Axis& axis)
{
    if ((axis == Axis::AllAxes()) || (axis == Axis::AllStaticAxes()))
        LogicError("AllAxes and AllStaticAxes are currently not supported.");

    if (axis.IsSequenceAxis())
        LogicError("Sequence axis are currently not supported.");

    if (axis.IsBatchAxis())
        return 0;

    return axis.StaticAxisIndex() + 1;
}

onnx::TypeProto CNTKToONNXHelper::ToTypeProto(const NDShape& shape, bool hasBatchAxis, bool hasSequenceAxis, bool doReverseShape)
{
    // !! IMP: This method has been modified quite a bit. Like the introduction of doReverseShape. 
    // Make sure that it is merged correctly.

    onnx::TypeProto newShape;
    if (shape.HasInferredDimension())
    {
        LogicError("This model has tensor dimensions marked as InferredDimension. Please evaluate"
            "the model with test data at least once and try saving it again.");
    }

    // Sequence dimension should be before batch axis after we reverse the shape (reversal happens below).
    if (hasSequenceAxis)
        newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param("None");

    if (hasBatchAxis)
        newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(1);

    auto dimensions = shape.Dimensions();
    if (doReverseShape)
        dimensions = reverse(dimensions);
    for (auto dimension : dimensions)
    {
        if (dimension == NDShape::FreeDimension)
        {
            newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param("None");
        }
        else
        {
            newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(dimension);
        }
    }

    return newShape;
}

onnx::TypeProto CNTKToONNXHelper::ToTypeProto(const std::vector<bool>& shape)
{
    onnx::TypeProto newShape;
    auto dimensions = reverse(shape);
    for (auto dimension : dimensions)
        newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(dimension ? 1 : 0);

    return newShape;
}

onnx::TypeProto CNTKToONNXHelper::ToTypeProto(const std::vector<int>& shape,
    bool doReverseVec /* = true*/)
{
    onnx::TypeProto newShape;
    std::vector<int> dimensions(shape);
    if (doReverseVec)
        dimensions = reverse(dimensions);
    for (auto dimension : dimensions)
        newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(dimension);

    return newShape;
}

onnx::TypeProto CNTKToONNXHelper::ToTypeProto(const std::vector<Axis>& axes)
{
    std::vector<int> axesValue;
    for (auto axis : axes)
    {
        axesValue.push_back(ToIndex(axis));
    }
    std::sort(axesValue.begin(), axesValue.end());

    onnx::TypeProto newShape;
    for (auto dimension : axesValue)
        newShape.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(dimension);

    return newShape;
}

// this method is to undo an idempotent convertion in sanitize_permutation:
// Find the permutation such that when it is applied to the reverse
// of an input gives the reverse of perm applied to the input
// Example:
// input is[a, b, c, d], perm is[3, 0, 2, 1], perm of input is[d, a, c, b]
// we are looking for[2, 1, 3, 0] because when we apply it to[d, c, b, a]
// the result is[b, c, a, d] which is the revese of[d, a, c, b]
std::vector<int64_t> CNTKToONNXHelper::ConvertPermutationCNTKToONNX(const std::vector<Axis> &axes, bool hasBatchAxis)
{
    std::vector<int64_t> permutation(axes.size());
    for (int i = 0; i < axes.size(); i++)
    {
        int indexToONNXPermTable = axes.size() - i - 1;
        int axisIndexInCNTK = axes[i].StaticAxisIndex();
        int axisIndexInONNX = axes.size() - axisIndexInCNTK - 1;
        permutation[indexToONNXPermTable] = axisIndexInONNX;
    }
    if (hasBatchAxis)
    {
        for (int i = 0; i < permutation.size(); i++)
            permutation[i]++;
        permutation.insert(permutation.begin(), 0);
    }
    return permutation;
}

std::vector<float> CNTKToONNXHelper::INTSToVecFloat(const std::vector<int64_t> &ints)
{
    std::vector<float> vecFloat(ints.size());
    for (int i = 0; i < ints.size(); i++)
    {
        vecFloat[i] = (float)ints[i];
    }

    return vecFloat;
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const onnx::TypeProto& shape)
{
    std::vector<int64_t> newShape;

    for (int i = 0; i < shape.tensor_type().shape().dim_size(); i++)
        newShape.push_back((int64_t)shape.tensor_type().shape().dim(i).dim_value());

    return newShape;
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const NDShape& shape, bool hasBatchAxis)
{
    return ToINTS(ToTypeProto(shape, hasBatchAxis));
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const std::vector<bool>& shape)
{
    return ToINTS(ToTypeProto(shape));
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const std::vector<int>& shape,
    bool doReverseVec /* = true*/)
{
    return ToINTS(ToTypeProto(shape, doReverseVec));
}

std::vector<int64_t> CNTKToONNXHelper::ToINTS(const std::vector<Axis>& axes)
{
    return ToINTS(ToTypeProto(axes));
}

void CNTKToONNXHelper::UpdateONNXType(DataType dataType, onnx::TypeProto& type)
{
    switch (dataType)
    {
    case DataType::Float:
        type.mutable_tensor_type()->set_elem_type(onnx::TensorProto_DataType_FLOAT);
        break;
    case DataType::Double:
        type.mutable_tensor_type()->set_elem_type(onnx::TensorProto_DataType_DOUBLE);
        break;
    default:
        NOT_IMPLEMENTED;
    }
}

std::string CNTKToONNXHelper::ToOPName(const FunctionPtr& src)
{
    auto lookup = Operators::CntkToONNXLookup();
    assert(lookup.count(src->OpName()) != 0);

    std::string opName = ToString(src->OpName());
    if (lookup.count(src->OpName()) == 1)
    {
        auto attributesMap = lookup.find(src->OpName())->second.map;
        opName = attributesMap[src->OpName()];
    }
    else
    {
        // Some nodes map one to many.
        if (src->OpName() == L"Convolution")
        {
            auto transpose = (bool)src->Attributes()[L"transpose"].Value<bool>();
            if (transpose)
                opName = "ConvTranspose";
            else
                opName = "Conv";
        }
        else if (src->OpName() == L"Pooling")
        {
            PoolingType poolingType = (PoolingType)src->Attributes()[L"poolingType"].Value<size_t>();
            if (poolingType == PoolingType::Max)
                opName = "MaxPool";
            else
                opName = "AveragePool";
        }
        else if (src->OpName() == L"ReduceElements")
        {
            wstring cntkAttributeOpName = (wstring)src->Attributes()[PrimitiveFunction::AttributeNameReductionOpName].Value<wstring>();

            const AttributesMapping& attributeMap = Operators::FindAttributeMap(src->OpName(), cntkAttributeOpName);

            opName = attributeMap.map.at(cntkAttributeOpName);
        }
    }

    return opName;
}

// whether this op has any input with batch axis
bool CNTKToONNXHelper::OpInputsHasBatchAxis(const FunctionPtr& src)
{
    std::vector<Variable> inputs = src->Inputs();
    for (std::vector<Variable>::const_iterator it = inputs.cbegin(); it != inputs.cend(); it++)
    {
        if ((*it).HasBatchAxis())
            return true;
    }
    return false;
}

bool CNTKToONNXHelper::FilterInput(const FunctionPtr& src, const CNTK::Variable& input, size_t inputIndex)
{
    // In CNTK block functions, they expose all constants inside the block. For block functions that
    // map directly to ONNX OP, we don't care about constanst inside the block.
    if (input.IsConstant())
        return !Operators::IsValidInputs(src->OpName(), inputIndex);
    return false;
}

/*
CNTK python static axis is zero based. Free/Inferred axis is not static.
ONNX batch axis, if exists, is 0. in this case static axes start from 1.
CNTK cpp get static axis in a dis-normalized form (e.g. -axis - 1)
In general CNTK node attribute contains axis in this dis-normalized form.
This function converts dis-normalized form to ONNX form.
*/
int64_t CNTKToONNXHelper::ConvertAxisToOnnx(const Axis &axis, const Variable &operand)
{
    NDShape inputShape = operand.Shape();
    Axis normalizedAxis = NormalizeStaticAxis(const_cast<Axis &>(axis), inputShape.Rank());
    int64_t ax = inputShape.Rank() - normalizedAxis.StaticAxisIndex();
    if (!operand.HasBatchAxis())
        ax--;
    return ax;
}

std::vector<int64_t> CNTKToONNXHelper::ConvertAxesToOnnx(const std::vector<Axis> &axes, const Variable &operand)
{
    std::vector<int64_t> onnxAxes(axes.size());
    for (int i = 0; i < axes.size(); i++)
    {
        onnxAxes[i] = ConvertAxisToOnnx(axes[i], operand);
    }
    return onnxAxes;
}

/*
ONNX specifies braodcast for elementwise ops in following manners
shape(A) = (2, 3, 4, 5), shape(B) = (,), i.e. B is a scalar
shape(A) = (2, 3, 4, 5), shape(B) = (5,)
shape(A) = (2, 3, 4, 5), shape(B) = (4, 5)
shape(A) = (2, 3, 4, 5), shape(B) = (3, 4), with axis=1
shape(A) = (2, 3, 4, 5), shape(B) = (2), with axis=0

CNTK handles braodcast implicitely as numpy does. For example with above example 4,
the shape of the shall be:
(1, 3, 4, 1) or (3, 4, 1)

more general cases:
same rank:
case1: [a, b, c] + [1, b, 1] - broadcast
case1: [a, b, c] + [a, b, 1] - broadcast
case1: [a, b, c] + [1, b, c] - broadcast
case2: [1, b, 1] + [a, b, c] - swap to become case1 then broadcast
case2: [a, b, 1] + [a, b, c] - swap to become case1 then broadcast
case2: [1, b, c] + [a, b, c] - swap to become case1 then broadcast
case3: [a2, b, c2] + [a, b, c]: cannot broadcast

different ranks:
[a, b, c] + [b, 1]: reshape input[1] to [1, b, 1] to become case 1
[a, b, c] + [b, c]: reshape input[1] to [1, b, c] to become case 1

[b, 1] + [a, b, c]: reshape input[0] to [1, b, 1] to become case 2
[b, c] + [a, b, c]: reshape input[0] to [1, b, c] to become case 2

[a2, b, c2] + [b, c]: reshape input[1] to [1, b, c] to become case 3 (cannot broadcast)
[b, c2] + [a, b, c]: reshape input[0] to [1, b, c2] to become case 3 (cannot broadcast)

Note that there is an addition batch dimension at the front of the shape in ONNX.

*/
std::tuple<std::pair<std::vector<int>, std::vector<int>>, bool, int, bool> CNTKToONNXHelper::AdjustForBroadcastShape(
    const Variable &input1, const Variable &input2)
{
    bool broadcast;
    int axis = 0;
    NDShape shape1 = input1.Shape(), shape2 = input2.Shape();
    bool swapInput = false;

    bool hasAnyBatchAxis = input1.HasBatchAxis() || input2.HasBatchAxis();
    bool hasAnySequenceAxis = HasSequenceAxis(input1) || HasSequenceAxis(input2);

    // CNTK and ONNX dimensions are reversed.
    // Reverse the dimension so that broadcast and axis calculation is in ONNX sense.
    std::vector<int> dims1(reverse(Cast<size_t, int>(shape1.Dimensions())));
    std::vector<int> dims2(reverse(Cast<size_t, int>(shape2.Dimensions())));

    if ((shape1.TotalSize() > 1 && shape2.TotalSize() == 1) || (shape1.TotalSize() == 1 && shape2.TotalSize() > 1))
    {
        broadcast = true;
        swapInput = (shape1.TotalSize() == 1 && shape2.TotalSize() > 1);

        if (swapInput)
            std::swap(dims1, dims2);

        if (hasAnySequenceAxis)
            dims1.insert(dims1.begin(), 1);
        if (hasAnyBatchAxis)
            dims1.insert(dims1.begin(), 1);

        return make_tuple(std::pair<std::vector<int>, std::vector<int>>(dims1, dims2), broadcast, axis, swapInput);
    }

    if (shape1.Rank() < shape2.Rank())
    {
        // This is a case of [b, c] + [a, b, c].
        // Need to swap the inputs to fit into ONNX spec - only right-hand-side argument will be broadcasted.
        std::swap(dims1, dims2);
        swapInput = true;
    }

    if (dims1.size() > dims2.size())
    {
        // This is a case like [a, b, c] + [b, 1]. Make it [a, b, c] + [1, b, 1].
        dims2.insert(dims2.begin(), dims1.size() - dims2.size(), 1);
    }

    // Append batch dimension if needed.
    if (hasAnySequenceAxis)
    {
        dims1.insert(dims1.begin(), 1);
        dims2.insert(dims2.begin(), 1);
    }
    if (hasAnyBatchAxis)
    {
        dims1.insert(dims1.begin(), 1);
        dims2.insert(dims2.begin(), 1);
    }


    std::vector<int> broadcastShape;
    bool swapInputDueToDims;
    std::tie<std::vector<int>, bool, int>(dims2, broadcast, axis, swapInputDueToDims) = CalculateBroadcastAxis(dims1, dims2);

    if (broadcast && swapInput && swapInputDueToDims)
    {
        LogicError("Shapes of elementwise binary operation are not compatible.");
    }

    return make_tuple(std::pair<std::vector<int>, std::vector<int>>(dims1, dims2), broadcast, axis, swapInput || swapInputDueToDims);
}

/*
For example with:
case1: [a, b, c] + [ b, 1] - broadcast
broadcast shape = [b], broadcast = true, axis = 1
*/
std::tuple<std::vector<int>, bool, int, bool> CNTKToONNXHelper::CalculateBroadcastAxis(
    const std::vector<int> &dims1, const std::vector<int> &dims2)
{
    bool swapInput = false;
    // this method assumes dims1.size() == dims2.size(), which is granted by caller AdjustForBroadcastShape.
    bool broadCast = false;
    int axis_start = -1;
    int axis_stop = dims2.size();
    for (int i = 0; i < dims2.size(); i++)
    {
        if (dims1[i] != dims2[i])
        {
            if (dims1[i] == 1)
                swapInput = true;

            broadCast = true;
            if (axis_start != -1)
            {
                axis_stop = i;
                break;
            }
        }
        else
            if (dims2[i] != 1 && axis_start == -1)
            {
                axis_start = i;
            }
    }

    if (!broadCast)
    {
        return make_tuple(dims2, broadCast, axis_start, swapInput);
    }

    axis_start = axis_start > 0 ? axis_start : 0;

    const std::vector<int> broadcaseInputDims = swapInput ? dims1 : dims2;
    // sanity check;
    for (int i = 0; i < broadcaseInputDims.size(); i++)
    {
        if ((i < axis_start || i >= axis_stop) && broadcaseInputDims[i] != 1)
        {
            LogicError("dimension %d cannot be broadcasted", i);
        }
        else if (i >= axis_start && i < axis_stop && dims1[i] != dims2[i])
        {
            LogicError("dimension %d cannot be broadcasted", i);
        }
    }
    std::vector<int> dimensions;
    for (int i = axis_start; i < axis_stop; i++)
    {
        dimensions.push_back(broadcaseInputDims[i]);
    }

    return make_tuple(dimensions, broadCast, axis_start, swapInput);
}

//
// This is the main horsepower, it navigate CNTK graph recursivley while keep track of all visited nodes and variables, 
// and create the corresponding ONNX graph.
//
ONNXIR::Node* CNTKToONNXHelper::CreateNode(const FunctionPtr& src,
    ONNXIR::Graph* graph,
    std::unordered_map<FunctionPtr, ONNXIR::Node*>& functionNodes,
    std::unordered_map<Variable, ONNXIR::Node*>& variableNodes,
    const std::unordered_map<Variable, Variable>& compositeOutputsMap)
{
    auto iter = functionNodes.find(src);
    if (iter != functionNodes.end())
        return iter->second;

    ONNXIR::Node* functionNode = nullptr;
    std::string opName = ToString(src->OpName());

    if (opName == "OptimizedRNNStack")
        return CreateONNXNodesForOptimizedRNNStack(src, graph, functionNodes, variableNodes, compositeOutputsMap);
    //
    // If this block node equivalent to a primitive ONNX OP, then treated as such.
    // And just maps its argument to ONNX node.
    //
    if (src->IsBlock() &&
        (!Operators::IsSupportedCNTKOP(src->OpName()) || Operators::IsLayerCNTKOP(src->OpName())))
    {
        functionNode = CreateNode(src->BlockRoot(), graph, functionNodes, variableNodes, compositeOutputsMap);
    }
    //
    // For compatibility of other framework that support ONNX, we will limit the list of OPs to the one
    // supported by ONNX https://github.com/onnx/onnx/tree/master/onnx/defs.
    //
    else if (Operators::IsSupportedCNTKOP(src->OpName()))
    {
        std::vector<ONNXIR::NodeArg> inputs;
        std::vector<ONNXIR::NodeArg> outputs;

        for (const auto& output : src->Outputs())
        {
            auto outputArgType = ToTypeProto(output.Shape(), output.HasBatchAxis());
            UpdateONNXType(output.GetDataType(), outputArgType);

            ONNXIR::NodeArg outputArg(ToString(output.Uid()), &outputArgType);
            outputs.push_back(outputArg);
        }

        for (size_t inputIndex = 0; inputIndex < src->Inputs().size(); ++inputIndex)
        {
            auto input = src->Inputs()[inputIndex];

            if (input.IsPlaceholder())
            {
                input = input.BlockFunctionVariableMapping();
                if (input.IsPlaceholder())
                    LogicError("Node '%S': Placeholder isn't supported currently.", src->AsString().c_str());
            }

            if (FilterInput(src, input, inputIndex))
                continue;

            //
            // Use user defined name if available otherwise use our internel unique name ID.
            //
            std::string inputName = ToString(input.Uid());
            auto inputItr = compositeOutputsMap.find(input);
            if (inputItr != compositeOutputsMap.end())
                inputName = ToString(inputItr->second.Uid());

            bool isConstant = (input.IsParameter() || input.IsConstant()) &&
                !Operators::IgnoreConstantAndParameter(src->OpName(), inputIndex);

            onnx::TypeProto inputArgType;

            if (Operators::SupportBroadcast(src->OpName()))
            {
                std::pair<std::vector<int>, std::vector<int>> adjustedDims;
                bool broadcast = false, swapInput = false;
                int axis = 0;
                int index0, index1;
                std::tie<int, int>(index0, index1) = Operators::GetElementWiseInputIndices(src->OpName());

                if (index0 != inputIndex && index1 != inputIndex)
                    continue;

                std::tie<std::pair<std::vector<int>, std::vector<int>>, bool, int, bool>(adjustedDims, broadcast, axis, swapInput) =
                    AdjustForBroadcastShape(src->Inputs()[index0], src->Inputs()[index1]);
                if (inputIndex == index0)
                    inputArgType = ToTypeProto(adjustedDims.first, false);
                else if (inputIndex == index1)
                    inputArgType = ToTypeProto(adjustedDims.second, false);
            }
            else if (opName == "Splice")
            {
                // for ops like Concat, batch axis may exist in one of the operand
                // CNTK allows the other operand(s) not having batch axis. But ONNX 
                // requires operands to have the same rank
                inputArgType = ToTypeProto(input.Shape(), OpInputsHasBatchAxis(src));
            }
            else if (opName == "Hardmax" || opName == "ImageScaler")
            {
                // ONNX specifies that hardmax, ImageScaler always need a batch axis
                inputArgType = ToTypeProto(input.Shape(), true);
            }
            else
            {
                if (isConstant && opName == "BatchNormalization" && (inputIndex > 0 && inputIndex <= 4)
                    && input.Shape().Rank() == 2)
                    // this is a workaround for brainscript models that have rank = 2 for BN inputs.
                    inputArgType = ToTypeProto(input.Shape().SubShape(0, input.Shape().Rank() - 1));
                else
                    inputArgType = ToTypeProto(input.Shape(), input.HasBatchAxis());
            }

            UpdateONNXType(input.GetDataType(), inputArgType);
            ONNXIR::NodeArg inputArg(inputName, &inputArgType);

            inputs.push_back(inputArg);

            //
            // Leaf nodes are data entry to the graph and need their own node with only output arg.
            //
            if (isConstant)
            {
                if (variableNodes.find(input) == variableNodes.end())
                {
                    std::vector<ONNXIR::NodeArg> varInputs;
                    std::vector<ONNXIR::NodeArg> varOutputs;

                    varOutputs.push_back({ inputArg });
                    ONNXIR::Node* variableNode = nullptr;
                    if (input.IsParameter() || input.IsConstant())
                    {
                        variableNode = graph->AddNode(inputName, "Constant", "", varInputs, varOutputs);
                        auto srcTensor = input.IsParameter() ? Parameter(input).Value() : Constant(input).Value();

                        onnx::TensorProto dstTensor;
                        CopyTensor(srcTensor, dstTensor, &inputArgType);

                        variableNode->AddAttribute("value", dstTensor);
                        variableNodes.emplace(input, variableNode);
                    }
                }
            }
            //
            // If this input is output, then it is the ouput of an up stream node. Recursively add all upstream nodes.
            // Pretty much, we are doing DFS.
            //
            else if (input.IsOutput())
                CreateNode(input.Owner(), graph, functionNodes, variableNodes, compositeOutputsMap);
        }

        //
        // Finally add a new node to ONNX graph.
        //
        functionNode = AddNode(src, graph, inputs, outputs);
    }
    else
        LogicError("Node '%S': Unsupported node.", src->AsString().c_str());

    functionNodes.emplace(src, functionNode);
    return functionNode;
}

void CNTKToONNXHelper::TraverseGraph(const FunctionPtr& src,
    std::set<FunctionPtr>& visited,
    std::unordered_map<Variable, Variable>& compositeOutputsMap)
{
    auto iter = visited.find(src);
    if (iter != visited.end())
        return;

    std::string opName = ToString(src->OpName());
    if (src->IsBlock() && (!Operators::IsSupportedCNTKOP(src->OpName()) || Operators::IsLayerCNTKOP(src->OpName())))
    {
        auto blockSrc = dynamic_cast<BlockFunction*>(src.get());
        for (auto map : blockSrc->CompositeOutputsMap())
            compositeOutputsMap.insert(map);
        TraverseGraph(src->BlockRoot(), visited, compositeOutputsMap);
    }
    else
    {
        for (auto input : src->Inputs())
        {
            if (input.IsPlaceholder())
            {
                input = input.BlockFunctionVariableMapping();
                if (input.IsPlaceholder())
                    LogicError("Node '%S': Placeholder isn't supported currently.", src->AsString().c_str());
            }

            if (input.IsOutput())
                TraverseGraph(input.Owner(), visited, compositeOutputsMap);
        }
    }

    visited.emplace(src);
}

void CNTKToONNXHelper::CopyAttributes(const FunctionPtr& src, ONNXIR::Node* node)
{
    auto lookup = Operators::CntkToONNXLookup();
    assert(lookup.count(src->OpName()) != 0);

    std::string opName = ToString(src->OpName());
    if (lookup.count(src->OpName()) == 1)
    {
        auto attributesMap = lookup.find(src->OpName())->second.map;
        opName = attributesMap[src->OpName()];

        if (src->OpName() == L"Clip")
        {
            if (src->Inputs().size() != 3)
            {
                LogicError("Clip should have 3 inputs.");
            }
            float minValue = src->Inputs()[1].Value()->AsScalar<float>();
            float maxValue = src->Inputs()[2].Value()->AsScalar<float>();
            node->AddAttribute("min", minValue);
            node->AddAttribute("max", maxValue);
        }
        if (src->OpName() == L"BatchNormalization")
        {
            auto spatial = (int64_t)((bool)src->Attributes()[L"spatial"].Value<bool>() ? 1 : 0);
            auto normalizationTimeConstant = (float)src->Attributes()[L"normalizationTimeConstant"].Value<double>();
            // auto blendTimeConstant = (float)src->Attributes()[L"blendTimeConstant"].Value<double>();
            auto epsilon = (float)src->Attributes()[L"epsilon"].Value<double>();

            //
            // onnx: running_mean = running_mean * momentum + mean * (1 - momentum)
            // cntk: expAvgFactor * MB stats + (1-expAvgFactor) * prev running stats
            //
            auto momentum = 0.0f;
            if (!isfinite(normalizationTimeConstant))
                momentum = 1.0f;
            else if (normalizationTimeConstant > 0)
                momentum = 1.0f + expm1(-48.0f / normalizationTimeConstant);

            node->AddAttribute(attributesMap[L"spatial"], spatial);
            node->AddAttribute("is_test", (int64_t)1);
            node->AddAttribute(attributesMap[L"epsilon"], epsilon);
            node->AddAttribute("momentum", momentum);
        }
        else if (src->OpName() == L"LocalResponseNormalization")
        {
            auto depthRadius = (int64_t)src->Attributes()[L"depthRadius"].Value<size_t>();
            auto bias = (float)src->Attributes()[L"bias"].Value<double>();
            auto alpha = (float)src->Attributes()[L"alpha"].Value<double>();
            auto beta = (float)src->Attributes()[L"beta"].Value<double>();

            node->AddAttribute(attributesMap[L"size"], depthRadius);
            node->AddAttribute(attributesMap[L"bias"], bias);
            node->AddAttribute(attributesMap[L"alpha"], alpha);
            node->AddAttribute(attributesMap[L"beta"], beta);
        }
        else if ((src->OpName() == L"LeakyReLU") || (src->OpName() == L"ELU"))
        {
            auto alpha = 0.01f;
            if (src->Attributes().Contains(L"alpha"))
                alpha = (float)src->Attributes()[L"alpha"].Value<double>();
            node->AddAttribute("alpha", alpha);
        }
        else if (src->OpName() == L"SELU")
        {
            auto alpha = 1.6732f;
            if (src->Attributes().Contains(L"alpha"))
                alpha = (float)src->Attributes()[L"alpha"].Value<double>();

            auto gamma = 1.0507f;
            if (src->Attributes().Contains(L"gamma"))
                gamma = (float)src->Attributes()[L"gamma"].Value<double>();

            node->AddAttribute("alpha", alpha);
            node->AddAttribute("gamma", gamma);
        }
        else if (src->OpName() == L"Dropout")
        {
            auto dropoutRate = (float)src->Attributes()[L"dropoutRate"].Value<double>();
            node->AddAttribute(attributesMap[L"dropoutRate"], dropoutRate);
            node->AddAttribute("is_test", (int64_t)1);
        }
        else if ((src->OpName() == L"RandomDistribution") ||
            (src->OpName() == L"UniformRandom") || (src->OpName() == L"NormalRandom") ||
            (src->OpName() == L"UniformRandomLike") || (src->OpName() == L"NormalRandomLike"))
        {
            auto randomArgs = AsVector<double>(src->Attributes()[L"randomDistributionArgs"].Value<std::vector<DictionaryValue>>());
            auto seed = (int64_t)src->Attributes()[L"rngSeed"].Value<int>();

            if ((src->OpName() == L"UniformRandom") || (src->OpName() == L"UniformRandomLike"))
            {
                node->AddAttribute("low", (float)randomArgs[0]);
                node->AddAttribute("high", (float)randomArgs[1]);
            }
            else
            {
                node->AddAttribute("mean", (float)randomArgs[0]);
                node->AddAttribute("scale", (float)randomArgs[1]);
            }

            node->AddAttribute(attributesMap[L"rngSeed"], seed);
            if ((src->OpName() == L"UniformRandom") || (src->OpName() == L"NormalRandom"))
            {
                auto shape = (NDShape)src->Attributes()[L"newShape"].Value<NDShape>();
                node->AddAttribute(attributesMap[L"newShape"], ToINTS(shape));
            }
        }
        else if ((src->OpName() == L"ReduceL1") || (src->OpName() == L"ReduceL2") || (src->OpName() == L"ReduceSumSquare"))
        {
            auto keepReducedDimensions = (int64_t)((bool)src->Attributes()[L"reductionKeepDimensions"].Value<bool>() ? 1 : 0);
            std::vector<Axis> reductionAxes;
            if (src->Attributes().Contains(L"axisVec"))
                reductionAxes = AsVector<Axis>(src->Attributes()[L"axisVec"].Value<std::vector<DictionaryValue>>());
            else if (src->Attributes().Contains(L"axis"))
                reductionAxes.push_back((Axis)(src->Attributes()[L"axis"].Value<Axis>()));

            node->AddAttribute(attributesMap[L"reductionKeepDimensions"], keepReducedDimensions);

            std::vector<int64_t> axes = ConvertAxesToOnnx(reductionAxes, src->Inputs()[0]);
            node->AddAttribute("axes", axes);
        }
        else if (src->OpName() == L"TransposeAxes")
        {
            if (src->Attributes().Contains(L"axisVec"))
            {
                std::vector<Axis> permutation = AsVector<Axis>(src->Attributes()[L"axisVec"].Value<std::vector<DictionaryValue>>());
                // CNTK permutation attribute is argsorted. Shall redo argsort (undo) to get the original python/ONNX perm attribute.
                std::vector<int64_t> perm = ConvertPermutationCNTKToONNX(permutation, src->Inputs()[0].HasBatchAxis());
                node->AddAttribute(attributesMap[L"axisVec"], perm);
            }
            else if (src->Attributes().Contains(L"axis1") && src->Attributes().Contains(L"axis2"))
            {
                // swapaxis: permutation is between two axes
                int rank = src->Output().Shape().Rank();
                std::vector<int64_t> perm;
                bool hasBatchAxis = src->Inputs()[0].HasBatchAxis();
                for (int index = 0; index < (hasBatchAxis ? (rank + 1) : rank); index++)
                {
                    perm.push_back(index);
                }

                Axis axis1 = (Axis)(src->Attributes()[L"axis1"].Value<Axis>()).StaticAxisIndex();
                Axis axis2 = (Axis)(src->Attributes()[L"axis2"].Value<Axis>()).StaticAxisIndex();
                int64_t axisIndex1 = ConvertAxisToOnnx(axis1, src->Inputs()[0]);
                int64_t axisIndex2 = ConvertAxisToOnnx(axis2, src->Inputs()[0]);
                std::swap(perm[axisIndex1], perm[axisIndex2]);
                node->AddAttribute(attributesMap[L"axisVec"], perm);
            }
        }
        else if (src->OpName() == L"Reshape")
        {
            auto shapeVec = src->Output().Shape().Dimensions();
            std::vector<int> newShapeVec;
            size_t numInferredDimensions(0);
            for (const auto& axisSize : shapeVec)
            {
                if (axisSize == NDShape::InferredDimension)
                {
                    numInferredDimensions++;
                    if (numInferredDimensions > 1)
                        LogicError("Reshape: Multiple InferredDimension not supported by ONNX.");
                    else
                        newShapeVec.push_back(-1);
                }
                else // REVIEW SPTIWARI: Should we fill 0 for FreeDimension here?
                    newShapeVec.push_back(static_cast<int>(axisSize));
            }
            // Always add a 1 to the shape for batch axis in ONNX tensors.
            if ((src->Inputs().size() > 0) && (src->Inputs()[0].HasBatchAxis()))
                newShapeVec.push_back(1);
            node->AddAttribute(attributesMap[L"shape"], ToINTS(newShapeVec));
        }
        else if (src->OpName() == L"Splice")
        {
            Axis axis = (Axis)(src->Attributes()[L"axis"].Value<Axis>());
            int64_t axisIndex = ConvertAxisToOnnx(axis, src->Inputs()[0]);
            node->AddAttribute(attributesMap[L"axis"], axisIndex);
        }
        else if (src->OpName() == L"Slice")
        {
            std::vector<int> beginIndex;
            std::vector<int> endIndex;

            if (src->Attributes().Contains(L"axisVec"))
            {
                std::vector<Axis> sliceAxes = AsVector<Axis>(src->Attributes()[L"axisVec"].Value<std::vector<DictionaryValue>>());
                node->AddAttribute(attributesMap[L"axes"], ToINTS(sliceAxes));

                beginIndex = AsVector<int>(src->Attributes()[L"beginIndexVec"].Value<std::vector<DictionaryValue>>());
                endIndex = AsVector<int>(src->Attributes()[L"endIndexVec"].Value<std::vector<DictionaryValue>>());
            }
            else if (src->Attributes().Contains(L"axis"))
            {
                Axis axis = (Axis)(src->Attributes()[L"axis"].Value<Axis>());
                int64_t axisIndex = ConvertAxisToOnnx(axis, src->Inputs()[0]);
                bool workaroundONNXRT = false;
                // this code is to workarund a LotusRT bug that fails
                // to take axes attribute into consideration.
                // we need to convert op attribute to a default ONNX case
                // where axes is not set (or set to ordered indices).
                if (workaroundONNXRT)
                {
                    bool hasBatchAxis = src->Inputs()[0].HasBatchAxis();
                    NDShape inputShape = src->Inputs()[0].Shape();
                    std::vector<int64_t> sliceAxes;
                    int numDims = hasBatchAxis ? (inputShape.Rank() + 1) : inputShape.Rank();
                    for (int onnxAxis = 0; onnxAxis < numDims; onnxAxis++)
                    {
                        sliceAxes.push_back(onnxAxis);
                        if (onnxAxis == 0 && hasBatchAxis)
                        {
                            // batch axis
                            beginIndex.push_back(0);
                            endIndex.push_back(1);
                        }
                        else
                        {
                            if (axisIndex == onnxAxis)
                            {
                                beginIndex.push_back((int)(src->Attributes()[L"beginIndex"].Value<int>()));
                                endIndex.push_back((int)(src->Attributes()[L"endIndex"].Value<int>()));
                            }
                            else
                            {
                                int cntkAxisIndex = numDims - onnxAxis - 1;
                                beginIndex.push_back(0);
                                endIndex.push_back(inputShape[cntkAxisIndex]);
                            }
                        }
                    }
                    node->AddAttribute(attributesMap[L"axes"], sliceAxes);
                }
                else
                {
                    std::vector<int64_t> sliceAxes;
                    sliceAxes.push_back(axisIndex);
                    node->AddAttribute(attributesMap[L"axes"], sliceAxes);

                    beginIndex.push_back((int)(src->Attributes()[L"beginIndex"].Value<int>()));
                    endIndex.push_back((int)(src->Attributes()[L"endIndex"].Value<int>()));
                }
            }

            std::vector<int64_t> beginIndex64 = Cast<int, int64_t>(beginIndex);
            std::vector<int64_t> endIndex64 = Cast<int, int64_t>(endIndex);

            node->AddAttribute(attributesMap[L"beginIndexVec"], beginIndex64);
            node->AddAttribute(attributesMap[L"endIndexVec"], endIndex64);
        }
        if (src->OpName() == L"Pad")
        {
            auto value = (float)src->Attributes()[L"paddingConstantValue"].Value<double>();
            auto mode = (size_t)src->Attributes()[L"paddingMode"].Value<size_t>();
            auto head = ToINTS(AsVector<size_t>(src->Attributes()[L"paddingHead"].Value<std::vector<DictionaryValue>>()));
            auto foot = ToINTS(AsVector<size_t>(src->Attributes()[L"paddingFoot"].Value<std::vector<DictionaryValue>>()));
            if (OpInputsHasBatchAxis(src))
            {
                head.insert(head.begin(), 0);
                foot.insert(foot.begin(), 0);
            }

            head.insert(head.end(), foot.begin(), foot.end());
            string modeStr;
            if (mode == 0)
                modeStr = "constant";
            else if (mode == 1)
                modeStr = "reflect";
            else if (mode == 2)
                NOT_IMPLEMENTED
            else
                LogicError("Invalid 'mode' value encountered in CNTK Pad node.");

            node->AddAttribute("mode", modeStr);
            node->AddAttribute("pads", head);
            if (mode == 0)
                node->AddAttribute("value", value);
        }
        else if (src->OpName() == L"DepthToSpace" || src->OpName() == L"SpaceToDepth")
        {
            size_t blockSize = src->Attributes()[L"blockSize"].Value<size_t>();
            node->AddAttribute("blocksize", static_cast<int64_t>(blockSize));
        }
        else if (src->OpName() == L"Softmax" || src->OpName() == L"LogSoftmax")
        {
            Axis axis = Axis(0);
            if (src->Attributes().Contains(L"axis"))
                axis = (Axis)(src->Attributes()[L"axis"].Value<Axis>());
            node->AddAttribute(attributesMap[L"axis"], (int64_t)ToIndex(axis));
        }
        else if (Operators::SupportBroadcast(src->OpName()))
        {
            std::pair<std::vector<int>, std::vector<int>> adjustedDims;
            bool broadcast = false, swapInput = false;
            int axis = 0;
            int index0, index1;
            std::tie<int, int>(index0, index1) = Operators::GetElementWiseInputIndices(src->OpName());
            std::tie<std::pair<std::vector<int>, std::vector<int>>, bool, int>(adjustedDims, broadcast, axis, swapInput) =
                AdjustForBroadcastShape(src->Inputs()[index0], src->Inputs()[index1]);

            node->AddAttribute("broadcast", (int64_t)(broadcast ? 1 : 0));
            if (broadcast && axis >= 0)
            {
                // +1 to take into consideration the batch aies
                node->AddAttribute("axis", (int64_t)axis);
            }
        }
        else if (src->OpName() == L"Times")
        {
            size_t outputRank = src->Attributes()[L"outputRank"].Value<size_t>();
            if (outputRank > 1)
                LogicError("Output rank other than 1 is not supported.");
        }
        else if (src->OpName() == L"ROIPooling")
        {
            auto roiOutputShape = (NDShape)src->Attributes()[L"roiOutputShape"].Value<NDShape>();
            auto ints = ToINTS(roiOutputShape, false);
            std::vector<float> pooled_shape = INTSToVecFloat(ints);

            auto spatialScale = (float)src->Attributes()[L"spatialScale"].Value<double>();

            node->AddAttribute("pooled_shape", pooled_shape);
            node->AddAttribute("spatial_scale", spatialScale);
        }
        else if (src->OpName() == L"HardSigmoid")
        {
            float alpha = (float)src->Attributes()[L"alpha"].Value<float>();
            float beta = (float)src->Attributes()[L"beta"].Value<float>();
            node->AddAttribute("alpha", alpha);
            node->AddAttribute("beta", beta);
        }
        else if (src->OpName() == L"Flatten")
        {
            Axis axis(0);
            if (src->Attributes().Contains(L"axis"))
            {
                axis = (Axis)(src->Attributes()[L"axis"].Value<Axis>());
            }
            int64_t ax = ConvertAxisToOnnx(axis, src->Inputs()[0]);
            node->AddAttribute(attributesMap[L"axis"], ax);
        }
        else if (src->OpName() == L"Squeeze")
        {
            std::vector<Axis> axes;
            if (src->Attributes().Contains(L"axisVec"))
            {
                axes = AsVector<Axis>(src->Attributes()[L"axisVec"].Value<std::vector<DictionaryValue>>());
            }
            else if (src->Attributes().Contains(L"axis"))
            {
                axes.push_back((Axis)(src->Attributes()[L"axis"].Value<Axis>()));
            }
            node->AddAttribute("axes", ToINTS(axes));
        }
        else if (src->OpName() == L"Gather")
        {
            if (src->Attributes().Contains(L"axis"))
            {
                Axis axis = (Axis)(src->Attributes()[L"axis"].Value<Axis>());
                int64_t ax = ConvertAxisToOnnx(axis, src->Inputs()[0]);
                node->AddAttribute(attributesMap[L"axis"], ax);
            }
        }
        else if (src->OpName() == L"ImageScaler")
        {
            float scale = (float)(src->Attributes()[L"Scaler"].Value<float>());
            std::vector<float> biases = AsVector<float>(src->Attributes()[L"Biases"].Value<std::vector<DictionaryValue>>());

            node->AddAttribute("scale", scale);
            node->AddAttribute("bias", biases);
        }
        else if (src->OpName() == L"MeanVarianceNormalization")
        {
            auto useStatsAcrossChannels = (int64_t)(src->Attributes()[L"useStatsAcrossChannels"].Value<bool>());
            auto doVarianceScaling = (int64_t)(src->Attributes()[L"doVarianceScaling"].Value<bool>());
            node->AddAttribute(attributesMap[L"useStatsAcrossChannels"], useStatsAcrossChannels);
            node->AddAttribute(attributesMap[L"doVarianceScaling"], doVarianceScaling);
        }
    }
    else
    {
        // Some nodes map one to many.
        if (src->OpName() == L"Convolution")
        {
            auto kernelShape = (NDShape)src->Attributes()[L"kernelShape"].Value<NDShape>();
            auto strides = (NDShape)src->Attributes()[L"strides"].Value<NDShape>();
            auto autoPadding = AsVector<bool>(src->Attributes()[L"autoPadding"].Value<std::vector<DictionaryValue>>());
            auto dilations = (NDShape)src->Attributes()[L"dilation"].Value<NDShape>();
            auto transpose = (bool)src->Attributes()[L"transpose"].Value<bool>();

            //
            // Remove the channel part for ONNX. This is because ONNX, unlike CNTK, does
            // not support padding (pads), dilation, or strides for channel dimension.
            kernelShape = kernelShape.SubShape(0, kernelShape.Rank() - 1);
            strides = strides.SubShape(0, strides.Rank() - 1);
            autoPadding.pop_back();
            dilations = dilations.SubShape(0, dilations.Rank() - 1);

            node->AddAttribute("kernel_shape", ToINTS(kernelShape));
            node->AddAttribute("strides", ToINTS(strides));
            node->AddAttribute("dilations", ToINTS(dilations));
            node->AddAttribute("group", (int64_t)1);

            if (transpose)
            {
                auto outputShape = (NDShape)src->Attributes()[L"outputShape"].Value<NDShape>();
                node->AddAttribute("output_shape", ToINTS(outputShape, src->Inputs()[1].HasBatchAxis()));
            }
            PutAutopadOrPadAttrInNode(node, autoPadding, kernelShape);
        }
        else if (src->OpName() == L"Pooling")
        {
            auto kernelShape = (NDShape)src->Attributes()[L"poolingWindowShape"].Value<NDShape>();
            auto strides = (NDShape)src->Attributes()[L"strides"].Value<NDShape>();
            bool ceilOutDim = (bool)src->Attributes()[L"ceilOutDim"].Value<bool>();
            if (strides.Rank() < kernelShape.Rank())
            {
                // TODO: Try removing this branch. May not be needed after batch dimension fix.
                strides = strides.AppendShape(NDShape(std::vector<size_t>(kernelShape.Rank() - strides.Rank(), 1)));
            }
            if ((strides.Rank() - kernelShape.Rank()) == 1)
            {
                // This can happen, for example, because a CNTK node includes strides for the channel axis as well. 
                strides = strides.SubShape(0, strides.Rank() - 1);
            }
            else if ((strides.Rank() - kernelShape.Rank()) > 1)
            {
                // This means that the length of kernel shape and strides is off by two or more which should not happen.
                LogicError("Node '%S': kernel shape and strides dimensionality does not match.", src->AsString().c_str());
            }
            auto autoPadding = AsVector<bool>(src->Attributes()[L"autoPadding"].Value<std::vector<DictionaryValue>>());

            node->AddAttribute("kernel_shape", ToINTS(kernelShape));
            node->AddAttribute("strides", ToINTS(strides));
            PutAutopadOrPadAttrInNode(node, autoPadding, kernelShape, ceilOutDim);
        }
        else if (src->OpName() == L"ReduceElements")
        {
            wstring cntkAttributeOpName = (wstring)src->Attributes()[PrimitiveFunction::AttributeNameReductionOpName].Value<wstring>();
            const AttributesMapping& attributeMap = Operators::FindAttributeMap(src->OpName(), cntkAttributeOpName);

            auto keepReducedDimensions = (int64_t)((bool)src->Attributes()[L"reductionKeepDimensions"].Value<bool>() ? 1 : 0);
            node->AddAttribute(attributeMap.map.at(L"reductionKeepDimensions"), keepReducedDimensions);

            if (src->Attributes().Contains(L"axisVec"))
            {
                std::vector<Axis> reductionAxes;
                reductionAxes = AsVector<Axis>(src->Attributes()[L"axisVec"].Value<std::vector<DictionaryValue>>());
                std::vector<int64_t> axes = ConvertAxesToOnnx(reductionAxes, src->Inputs()[0]);
                node->AddAttribute("axes", axes);
            }
            else if (src->Attributes().Contains(L"axis"))
            {
                // py axis -> cpp (-axis -1) -> normalize (rank + axis)
                Axis axis = (Axis)(src->Attributes()[L"axis"].Value<Axis>());
                int64_t ax = ConvertAxisToOnnx(axis, src->Inputs()[0]);

                node->AddAttribute("axis", ax);
            }
        }
    }
}

void CNTKToONNXHelper::PutAutopadOrPadAttrInNode(ONNXIR::Node* node,
    const std::vector<bool>& autoPadding, const NDShape& kernelShape, bool ceilOutDim)
{
    // Based on the CNTK node choose to put either the auto_pad or pads attribute in the ONNX node.

    // ONNX spec says that if 'pads' attributes is specified then 'VALID'
    // for 'auto_pad' is implied, and 'auto_pad' attribute should not (must not)
    // be explicitly specified/set.
    bool isExplicitPadValueNeeded = std::find(autoPadding.begin(), autoPadding.end(), false) != autoPadding.end();
    if (isExplicitPadValueNeeded && !ceilOutDim)
    {
        auto padsValueVectorsForONNX = GetONNXPadsAttributeFromCNTKNode(autoPadding, kernelShape, ceilOutDim);
        auto lowerPads = ToINTS(padsValueVectorsForONNX.first);
        auto upperPads = ToINTS(padsValueVectorsForONNX.second);
        lowerPads.insert(lowerPads.end(), upperPads.cbegin(), upperPads.cend());
        node->AddAttribute("pads", lowerPads);
    }
    else if (ceilOutDim)
        node->AddAttribute("auto_pad", "SAME_LOWER");
    else
        node->AddAttribute("auto_pad", "SAME_UPPER");
}

std::vector<ONNXIR::NodeArg> CNTKToONNXHelper::MapInputsOrderToONNX(const FunctionPtr& src, const std::vector<ONNXIR::NodeArg>& inputs)
{
    if (Operators::HasInputIndexMap(src->OpName()))
    {
        std::vector<ONNXIR::NodeArg> orderedInputs;
        std::map<int, ONNXIR::NodeArg> orderedInputsMap;
        auto map = Operators::ToONNXInputIndexMap(src->OpName());

        for (size_t inputIndex = 0; inputIndex < inputs.size(); ++inputIndex)
        {
            if (map[inputIndex] >= 0)
                orderedInputsMap.insert(std::pair<int, ONNXIR::NodeArg>(map[inputIndex], inputs[inputIndex]));
        }

        for (const auto& item : orderedInputsMap)
            orderedInputs.push_back(item.second);

        return orderedInputs;
    }

    return inputs;
}

ONNXIR::Node* FindByName(ONNXIR::Graph* graph, const std::string &name)
{
    for (ONNXIR::Graph::NodeIterator it = graph->Nodes_begin(); it != graph->Nodes_end(); ++it)
    {
        ONNXIR::Node *node = *it;

        const std::vector<ONNXIR::NodeArg>& outputNodeArgs = node->OutputDefs();
        for (int i = 0; i < outputNodeArgs.size(); i++)
        {
            if (outputNodeArgs[i].Name() == name)
            {
                return node;
            }
        }
    }
    return nullptr;
}

ONNXIR::Node* CNTKToONNXHelper::AddNode(const FunctionPtr& src, ONNXIR::Graph* graph, const std::vector<ONNXIR::NodeArg>& inputs, const std::vector<ONNXIR::NodeArg>& outputs)
{
    ONNXIR::Node* node = nullptr;
    auto orderedInputs = MapInputsOrderToONNX(src, inputs);
    auto nodeName = src->Name().empty() ? ToString(src->Uid()) : ToString(src->Name());

    if (Operators::SupportBroadcast(src->OpName()))
    {
        // when converting CNTK to ONNX with broadcasting, the boardcasting input at right-hand-side
        // needs to be reshaped. Reshape is not needed if the broadcasting input is a constant. In such case
        // CreateNode already created a constant with the needed shape. 
        // If the broadcasting input is not a constant, a reshape operation needs to be inserted. 
        // The following code does this reshape insertion.
        const TensorShapeProto* input1Shape = orderedInputs[0].Shape();
        const TensorShapeProto* input2Shape = orderedInputs[1].Shape();
        int input1Rank = input1Shape->dim_size();
        int input2Rank = input2Shape->dim_size();
        ONNXIR::Node* inputNode2 = FindByName(graph, orderedInputs[1].Name());
        if (input2Rank < input1Rank && inputNode2 != nullptr && inputNode2->OpType() != "Constant" && input2Rank != 0)
        {
            // The conditions for inserting a reshape op (the if statement logic above) are:
            // 1. input2Rank < input1Rank : Broadcast is needed. 
            // 2. inputNode2->OpType() != "Constant" : Because if it is Constant we create a 
            //    node for it explicitly in CreateNode() method above.
            // 3. input2Rank != 0 : That is, the second input is not a scalar. If it is then
            //    Reshape is not needed.
            ONNXIR::NodeArg inputOutput2Arg(orderedInputs[1].Name() + string("_reshape1"), nullptr);
            inputOutput2Arg.SetShape(*input2Shape);

            auto reshapeNode2 = graph->AddNode(nodeName + string("_reshape1"), "Reshape", "", { orderedInputs[1] }, { inputOutput2Arg });

            onnx::TypeProto reshapeTypeProto2 = TensorShapeProtoToTypeProto(input2Shape);

            reshapeNode2->AddAttribute("shape", ToINTS(reshapeTypeProto2));

            node = graph->AddNode(nodeName, ToOPName(src), "", { orderedInputs[0] , inputOutput2Arg }, outputs);
        }
        else
        {
            node = graph->AddNode(nodeName, ToOPName(src), "", orderedInputs, outputs);
        }
    }
    else
    {
        //
        // CNTK Times OP is way more flexible for ONNX, so depend on the inputs and output shape,
        // we will need to insert some reshapes.
        //
        if (src->OpName() == L"Times")
        {
            auto input1Shape = orderedInputs[0].Shape();
            auto input2Shape = orderedInputs[1].Shape();
            auto outputShape = outputs[0].Shape();

            int input1Rank = input1Shape->dim_size();
            int input2Rank = input2Shape->dim_size();
            int outputRank = outputShape->dim_size();
            int reductionRank = (input1Rank + input2Rank - outputRank) / 2;

            if (reductionRank > 1) // We need to insert reshape.
            {
                auto input1Reshape = ReduceRank(input1Shape, reductionRank, true);
                auto input2Reshape = ReduceRank(input2Shape, reductionRank, false);

                UpdateONNXType(src->Inputs()[1].GetDataType(), input1Reshape);
                UpdateONNXType(src->Inputs()[0].GetDataType(), input2Reshape);

                ONNXIR::NodeArg inputOutput1Arg(orderedInputs[0].Name() + string("_reshape0"), &input1Reshape);
                ONNXIR::NodeArg inputOutput2Arg(orderedInputs[1].Name() + string("_reshape1"), &input2Reshape);

                auto reshapeNode1 = graph->AddNode(nodeName + string("_reshape0"), "Reshape", "", { orderedInputs[0] }, { inputOutput1Arg });
                auto reshapeNode2 = graph->AddNode(nodeName + string("_reshape1"), "Reshape", "", { orderedInputs[1] }, { inputOutput2Arg });

                reshapeNode1->AddAttribute("shape", ToINTS(input1Reshape));
                reshapeNode2->AddAttribute("shape", ToINTS(input2Reshape));

                node = graph->AddNode(nodeName, ToOPName(src), "", { inputOutput1Arg , inputOutput2Arg }, outputs);
            }
            else
                node = graph->AddNode(nodeName, ToOPName(src), "", orderedInputs, outputs);
        }
        else if (src->OpName() == L"LayerNormalization")
        {
            // Special handling of LayerNormalization to use MeanVarianceNormalization (and not reduce* ops).

            // This assumes that the orderedInputs are in the order:
            // [0]: tensor operand, [1]: scale constant, [2]: bias constant.
            // Also assumes that tensor operand is index [2] in src->Inputs(). 
            auto input0 = orderedInputs[0];
            onnx::TypeProto input0ArgType = ToTypeProto(src->Inputs()[2].Shape(), src->Inputs()[2].HasBatchAxis());
            UpdateONNXType(src->Inputs()[2].GetDataType(), input0ArgType);
            ONNXIR::NodeArg mvnTensorOutputArg(nodeName + string("_mvn_output0"), &input0ArgType);
            ONNXIR::Node* mvnNode = graph->AddNode(nodeName + string("_MVN"), "MeanVarianceNormalization",
                "", { input0 }, { mvnTensorOutputArg });
            mvnNode->AddAttribute("across_channels", static_cast<int64_t>(1));
            mvnNode->AddAttribute("normalize_variance", static_cast<int64_t>(1));

            auto input1 = orderedInputs[1];
            ONNXIR::NodeArg mulTensorOutputArg(nodeName + string("_mul_output0"), &input0ArgType);
            ONNXIR::Node* mulNode = graph->AddNode(nodeName + string("_mul"), "Mul",
                "", { mvnTensorOutputArg, input1 }, { mulTensorOutputArg });
            mulNode->AddAttribute("broadcast", static_cast<int64_t>(1));

            auto input2 = orderedInputs[2];
            ONNXIR::NodeArg addTensorOutputArg(nodeName + string("_add_output0"), &input0ArgType);
            node = graph->AddNode(nodeName + string("_add"), "Add",
                "", { mulTensorOutputArg, input2 }, { addTensorOutputArg });
            node->AddAttribute("broadcast", static_cast<int64_t>(1));
        }
        else
            node = graph->AddNode(nodeName, ToOPName(src), "", orderedInputs, outputs);
    }

    //
    // Copy and validate attributes.
    //
    CopyAttributes(src, node);

    return node;
}

std::pair<std::vector<int>, std::vector<int> > CNTKToONNXHelper::GetONNXPadsAttributeFromCNTKNode(
    const std::vector<bool>& cntkAutoPadding, const NDShape& kernelShape, bool ceilOutDim)
{
    // Figure out the value for 'pads' ONNX attribute.

    // Only one of the two ONNX conv attributes, auto_pad and pads, can be specified in the saved model. 
    // It is assumed at this point that we need an explicit padding vector, pads, and not the auto_pad attribute. 
    // The 'auto_pad' atrribute is implied to be 'VALID' by ONNX specification if the 'pads' attribute is specified
    // (padsValueVector) for the dimensions for which cntkAutoPadding is true.
    assert(kernelShape.Rank() == cntkAutoPadding.size());
    std::vector<int> padsValueVectorLower(kernelShape.Rank(), 0);
    std::vector<int> padsValueVectorUpper(kernelShape.Rank(), 0);
    for (size_t i = 0; i < cntkAutoPadding.size(); ++i)
    {
        if (!cntkAutoPadding[i]) continue;
        auto q = kernelShape[i] / 2;
        padsValueVectorLower[i] = kernelShape[i] % 2 ? q : (q - 1);
        padsValueVectorUpper[i] = q;
    }
    return std::make_pair(padsValueVectorLower, padsValueVectorUpper);
}

ONNXIR::Node* CNTKToONNXHelper::CreateONNXNodesForOptimizedRNNStack(const FunctionPtr &src,
    ONNXIR::Graph* graph,
    std::unordered_map<FunctionPtr, ONNXIR::Node*>& functionNodes,
    std::unordered_map<Variable, ONNXIR::Node*>& variableNodes,
    const std::unordered_map<Variable, Variable>& compositeOutputsMap)
{
    auto numLayers = (size_t)src->Attributes()[L"numLayers"].Value<size_t>();
    auto hiddenSize = (size_t)src->Attributes()[L"hiddenSize"].Value<size_t>();
    auto bidirectional = (bool)(src->Attributes()[L"bidirectional"].Value<bool>());
    auto recurrentOp = (wstring)src->Attributes()[L"recurrentOp"].Value<wstring>();

    size_t numDirections = bidirectional ? 2 : 1;
    size_t inputSize = src->Inputs()[0].Shape()[0];
    auto Wcombined = src->Inputs()[1];
    auto WcombinedShape = Wcombined.Shape();

    // Step 1: Read out the OptimzedRNNStack input weight matrix (the big one that combines all weights and biases).
    NDArrayViewPtr srcTensor = Wcombined.IsParameter() ? Parameter(Wcombined).Value() : Constant(Wcombined).Value();
    NDArrayViewPtr srcTemp = srcTensor->DeepClone();
    // This is our own copy so move it to the CPU.
    srcTemp->ChangeDevice(DeviceDescriptor::CPUDevice());
    float* Wdata = srcTemp->WritableDataBuffer<float>();
    // ?? Does Matrix ctor below make a copy of the buffer. If yes, then we don't need to do DeepClone above.
    Matrix<float> Wm(WcombinedShape[0], WcombinedShape[1], Wdata, -1, MatrixType::DENSE, MatrixFormat::matrixFormatDense); // -1 denotes CPU. // matrixFormatDenseRowMajor
    printf("\n Wm--------------\n");
    Wm.Print();
    printf("\n ---------------------------------\n");
                                                                      // Step 2: Extract individual weight and bias matrices for each layer from the big weight matrix.
    std::vector<NDArrayViewPtr> W, R, B;
    std::tie<std::vector<NDArrayViewPtr>, std::vector<NDArrayViewPtr>, std::vector<NDArrayViewPtr> >
        (W, R, B) = SplitOptimzedRnnWtoIndivMats(Wm, numLayers, inputSize, hiddenSize, bidirectional, recurrentOp);

    /*printf("\n W[0] final--------------\n");
    PrintNDArrayView(W[0]);
    printf("--------------------------------\n");
    printf("\n W[1] final--------------\n");
    PrintNDArrayView(W[1]);
    printf("--------------------------------\n");
    printf("\n R[0] final--------------\n");
    PrintNDArrayView(R[0]);
    printf("--------------------------------\n");
    printf("\n R[1] final--------------\n");
    PrintNDArrayView(R[1]);
    printf("--------------------------------\n");
    printf("\n B0] final--------------\n");
    PrintNDArrayView(B[0]);
    printf("--------------------------------\n");
    printf("\n B[1] final--------------\n");
    PrintNDArrayView(B[1]);
    printf("--------------------------------\n");*/
    // Step 3: Create ONNX nodes mirroring the implementation of OptimizedRNNStack.
    ONNXIR::Node* functionNode;
    bool inputNeedsShapeAdapter(false);
    auto ornnInput = src->Inputs()[0]; // CNTK OptimizedRNNStack node's input operand.
    auto ornnInputArgType = ToTypeProto(ornnInput.Shape(), ornnInput.HasBatchAxis(), HasSequenceAxis(ornnInput));
    UpdateONNXType(ornnInput.GetDataType(), ornnInputArgType);
    auto ornnOutput = src->Outputs()[0];
    // auto ornnOutputArgType = ToTypeProto(ornnOutput.Shape(), ornnOutput.HasBatchAxis(), HasSequenceAxis(ornnOutput));
    auto outArgType1 = ToTypeProto({ ornnOutput.Shape()[0] / numDirections, numDirections }, ornnOutput.HasBatchAxis(), HasSequenceAxis(ornnOutput));
    TensorShapeProto outArgShape = outArgType1.mutable_tensor_type()->shape();
    int opRank = outArgShape.dim_size();
    std::vector<int> x(opRank);
    std::iota(x.begin(), x.end(), 0);
    std::swap(x[opRank - 2], x[opRank - 3]); // swap (last but one) annd (last but two)
    onnx::TypeProto ornnOutputArgType;
    for (int index = 0; index < opRank; index++)
    {
        if (outArgShape.dim(x[index]).has_dim_param()) // For sequence axis, which is a dynamic axis.
            ornnOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param(outArgShape.dim(x[index]).dim_param());
        else
            ornnOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(outArgShape.dim(x[index]).dim_value());
    }
    UpdateONNXType(ornnOutput.GetDataType(), ornnOutputArgType);

    ONNXIR::NodeArg layerInputOperandArg(ToString(ornnInput.Uid()), &ornnInputArgType);
    for (size_t i = 0; i < numLayers; ++i)
    {
        std::vector<ONNXIR::NodeArg> inputs;
        std::vector<ONNXIR::NodeArg> outputs;

        // Input operand X
        if (inputNeedsShapeAdapter)
        {
            std::string adapterBasename = (src->Name().empty() ? ToString(src->Uid()) : ToString(src->Name())) + "_Adapter_" + std::to_string(i);
            ONNXIR::NodeArg shapeAdaptedInputOperandArg = LSTMOutputShapeAdapter(layerInputOperandArg, ornnOutputArgType, graph,
                numDirections, hiddenSize, ornnOutput.GetDataType(), adapterBasename);
            inputs.push_back(shapeAdaptedInputOperandArg);
        }
        else
            inputs.push_back(layerInputOperandArg);

        // Input weight tensor W
        auto WArgName = ToString(Wcombined.Uid()) + "_W_" + std::to_string(i);
        printf("\n Layer %zu - W After--------------\n", i);
        PrintNDArrayView(W[i]);
        printf("--------------------------------\n");
        CreateRecurrentWeightONNXNodes(graph, variableNodes, Wcombined, inputs, W[i], WArgName);
        // Input weight tensor R (equivalent to CNTK's H)
        auto RArgName = ToString(Wcombined.Uid()) + "_R_" + std::to_string(i);
        printf("\n Layer %zu - R After--------------\n", i);
        PrintNDArrayView(R[i]);
        printf("--------------------------------\n");
        CreateRecurrentWeightONNXNodes(graph, variableNodes, Wcombined, inputs, R[i], RArgName);
        // Input weight tensor B
        auto BArgName = ToString(Wcombined.Uid()) + "_B_" + std::to_string(i);
        printf("\n Layer %zu - B After--------------\n", i);
        PrintNDArrayView(B[i]);
        printf("--------------------------------\n");
        CreateRecurrentWeightONNXNodes(graph, variableNodes, Wcombined, inputs, B[i], BArgName);
        // Create dummy input arg for optional arg sequence_lens
        auto seqLenArgName = ToString(Wcombined.Uid()) + "_seq_len_" + std::to_string(i);
        onnx::TypeProto inputSeqLenArgType = ToTypeProto(std::vector<int>({ 1 }), false);
        inputSeqLenArgType.mutable_tensor_type()->set_elem_type(onnx::TensorProto_DataType_INT32);
        ONNXIR::NodeArg inputArg_sequence_lens(seqLenArgName, &inputSeqLenArgType);
        inputs.push_back(inputArg_sequence_lens);
        // Create dummy input arg for optional arg initial_h
        auto initalHArgName = ToString(Wcombined.Uid()) + "_initial_h_" + std::to_string(i);
        ONNXIR::NodeArg inputArg_initial_h(initalHArgName, nullptr);
        inputs.push_back(inputArg_initial_h);
        // Create dummy input arg for optional arg initial_c
        auto initalCArgName = ToString(Wcombined.Uid()) + "_initial_c_" + std::to_string(i);
        ONNXIR::NodeArg inputArg_initial_c(initalCArgName, nullptr);
        inputs.push_back(inputArg_initial_c);
        // Create dummy input arg for optional arg P (peepholes)
        auto initalPArgName = ToString(Wcombined.Uid()) + "_P_" + std::to_string(i);
        ONNXIR::NodeArg inputArg_P(initalPArgName, nullptr);
        inputs.push_back(inputArg_P);

        // Output arguments
        int64_t outputSequence = 1; // For now, we always output Y. So this attribute value is 1.
        auto outArgName = ToString(Wcombined.Uid()) + "_Output_Y_" + std::to_string(i);
        ONNXIR::NodeArg outputArg_Y(outArgName, &ornnOutputArgType);
        outputs.push_back(outputArg_Y);

        // Dummy output arg Y_h
        auto outputYhArgName = ToString(Wcombined.Uid()) + "_Output_Y_h_" + std::to_string(i);
        // ONNXIR::NodeArg outputArg_Yh(outputYhArgName, nullptr);
        auto outputYhArgType = ToTypeProto(std::vector<int>({ 1, 1, static_cast<int>(hiddenSize) }), false);
        UpdateONNXType(ornnOutput.GetDataType(), outputYhArgType);
        ONNXIR::NodeArg outputArg_Yh(outputYhArgName, &outputYhArgType);
        outputs.push_back(outputArg_Yh);

        // Dummy output arg Y_c
        auto outputYcArgName = ToString(Wcombined.Uid()) + "_Output_Y_c_" + std::to_string(i);
        // ONNXIR::NodeArg outputArg_Yc(outputYcArgName, nullptr);
        auto outputYcArgType = ToTypeProto(std::vector<int>({ 1, 1, static_cast<int>(hiddenSize) }), false);
        UpdateONNXType(ornnOutput.GetDataType(), outputYcArgType);
        ONNXIR::NodeArg outputArg_Yc(outputYcArgName, &outputYcArgType);
        outputs.push_back(outputArg_Yc);

        auto rnnNodeName = (src->Name().empty() ? ToString(src->Uid()) : ToString(src->Name())) + std::to_string(i);
        functionNode = graph->AddNode(rnnNodeName, "LSTM", "", inputs, outputs);

        std::vector<std::string> singleDirectionActivation({ "Sigmoid", "Tanh", "Tanh" }); // REVIEW: Check this is the order.
        std::vector<std::string> activations;
        activations.insert(activations.end(), singleDirectionActivation.begin(), singleDirectionActivation.end());
        if (bidirectional)
            activations.insert(activations.end(), singleDirectionActivation.begin(), singleDirectionActivation.end());
        functionNode->AddAttribute("activations", activations);
        functionNode->AddAttribute("direction", bidirectional ? "bidirectional" : "forward");
        functionNode->AddAttribute("hidden_size", (int64_t)hiddenSize);
        functionNode->AddAttribute("output_sequence", outputSequence);

        layerInputOperandArg = outputArg_Y; // Output of this layer is the input to the next layer in the loop.
        inputNeedsShapeAdapter = true; // To enable shapoe adapter to allow stacking for next layer. 
    }

    if (ornnInput.IsOutput())
        CreateNode(ornnInput.Owner(), graph, functionNodes, variableNodes, compositeOutputsMap);

    functionNodes.emplace(src, functionNode);
    return functionNode;
}

std::tuple<std::vector<NDArrayViewPtr>, std::vector<NDArrayViewPtr>, std::vector<NDArrayViewPtr> >
CNTKToONNXHelper::SplitOptimzedRnnWtoIndivMats(Matrix<float>& WbigIn,
    size_t numLayers, size_t inputSize, size_t hiddenSize, bool bidirectional, wstring recurrentOp)
{
    std::vector<NDArrayViewPtr> onnxInputTensor(3);
    size_t numDirections = bidirectional ? 2 : 1;
    size_t numGates;
    if (recurrentOp == L"lstm")
        numGates = 4;
    else if (recurrentOp == L"rnnReLU" || recurrentOp == L"rnnTanh")
        numGates = 1;
    else
        InvalidArgument("Unsupported recurrent op value.");

    std::vector<Matrix<float> >  W;
    std::vector<Matrix<float> >  R;
    std::vector<Matrix<float> >  B;

    // The next two operations will make a deep copy and flatten the matrix
    // in the same order as the Python matrix Wbig (row-major).
    Matrix<float> Wbig = WbigIn.Transpose(); // Deep copy.
    Wbig.Reshape(1, WbigIn.GetNumElements()); 
    //// Matrix<float> Wbig(1, WbigIn.GetNumElements(), -1, MatrixType::DENSE, MatrixFormat::matrixFormatDense); // -1 is DEVICE_ID for CPU.
    //Wbig.SetValue(Wbig.Reshaped(1, WbigIn.GetNumElements())); //Deep copy, preserving row-major order of the underlying buffer.
    printf("\n Wbig flattened.--------------\n");
    Wbig.Print();
    printf("\n ---------------------------------\n");

    // Step 2: Extracting the weights W and R from big weight matrix (including backward ones in case of bidirectional op).
    size_t offset(0);
    size_t layerInputSize(inputSize);
    for (size_t i = 0; i < numLayers; ++i)
    {
        Matrix<float> fW = GetWeightMatFromOrnnBigW(Wbig, offset, layerInputSize, hiddenSize, numGates, recurrentOp);
        printf("\nLayer %zu - fW--------\n", i);
        fW.Print();
        printf("\n---------------------\n");
        offset += layerInputSize * hiddenSize * numGates;
        W.push_back(Matrix<float>(fW, -1));
        Matrix<float> fR = GetWeightMatFromOrnnBigW(Wbig, offset, hiddenSize, hiddenSize, numGates, recurrentOp);
        fR = GetWeightMatFromOrnnBigW(Wbig, offset, hiddenSize, hiddenSize, numGates, recurrentOp);
        offset += hiddenSize * hiddenSize * numGates;
        R.push_back(Matrix<float>(fR, -1));
        printf("\nLayer %zu - fR--------\n", i);
        fR.Print();
        printf("\n---------------------\n");

        // Matrix<float> bW(-1), bR(-1); // deviceId = 0 for CPU.
        if (bidirectional)
        {
            Matrix<float> bW = GetWeightMatFromOrnnBigW(Wbig, offset, layerInputSize, hiddenSize, numGates, recurrentOp);
            printf("\n Layer %zu - bW--------\n", i);
            bW.Print();
            printf("\n ---------------------\n");
            offset += layerInputSize * hiddenSize * numGates;
            W.push_back(Matrix<float>(bW, -1));
            Matrix<float> bR = GetWeightMatFromOrnnBigW(Wbig, offset, hiddenSize, hiddenSize, numGates, recurrentOp);
            offset += hiddenSize * hiddenSize * numGates;
            R.push_back(Matrix<float>(bR, -1));
            printf("\n Layer %zu - bR--------\n", i);
            bR.Print();
            printf("\n ---------------------\n");
        }

        layerInputSize = hiddenSize * numDirections;
    }

    // Step 3: Extracting the biases B from big weight matrix (including backward ones in case of bidirectional op).
    // NOTE: that 'offset' should be set correctly based on the extraction of weight matrices W and R as in Step 2.
    // In Step 3 we cannot start with offset = 0 for biases, since they start from somewhere in the middle of the
    // big weight matrix.
    for (size_t i = 0; i < numLayers; ++i)
    {
        Matrix<float> fB = GetBiasMatFromOrnnBigW(Wbig, offset, hiddenSize, numGates, recurrentOp);
        printf("Layer %zu - fB--------\n", i);
        fB.Print();
        printf("---------------------\n");
        offset += 2 * hiddenSize * numGates;
        B.push_back(Matrix<float>(fB, -1));
        if (bidirectional)
        {
            Matrix<float> bB = GetBiasMatFromOrnnBigW(Wbig, offset, hiddenSize, numGates, recurrentOp);
            offset += 2 * hiddenSize * numGates;
            B.push_back(Matrix<float>(bB, -1));
            printf("Layer %zu - bB--------\n", i);
            bB.Print();
            printf("---------------------\n");
        }
    }

    // Step 4: Convert weight matrices into NDArrayView;
    std::vector<NDArrayViewPtr> Wonnx = ToRnnWeightPerLayerOnnxFormat(W, numLayers, numDirections, numGates, hiddenSize, inputSize, true);
    std::vector<NDArrayViewPtr> Ronnx = ToRnnWeightPerLayerOnnxFormat(R, numLayers, numDirections, numGates, hiddenSize, hiddenSize, false);

    // Step 5: Convert bias matrices into NDArrayView;
    std::vector<NDArrayViewPtr> Bonnx = ToRnnBiasPerLayerOnnxFormat(B, numLayers, numDirections, hiddenSize, numGates);

    /*printf("\n Bonnx[0] outside--------------\n");
    PrintNDArrayView(Bonnx[0]);
    printf("--------------------------------\n");

    printf("\n Bonnx[1] outside--------------\n");
    PrintNDArrayView(Bonnx[1]);
    printf("--------------------------------\n");
*/

    return std::make_tuple(std::move(Wonnx), std::move(Ronnx), std::move(Bonnx));
}

Matrix<float> CNTKToONNXHelper::GetWeightMatFromOrnnBigW(Matrix<float>& Wbig, size_t offset,
    size_t layerInputSize, size_t layerOutputSize, size_t numGates, wstring recurrentOp)
{
    //Matrix<float> W0 = Wbig.ColumnSlice(offset, layerInputSize*layerOutputSize*numGates);
    Matrix<float> W0(-1);
    W0.SetValue(Wbig.ColumnSlice(offset, layerInputSize*layerOutputSize*numGates));
    W0.Reshape(layerInputSize, layerOutputSize*numGates);
    //printf("\n---------W0--------\n");
    //W0.Print();
    //printf("---------------------\n");
    // auto W = W0.Transpose(); // Confirmed: This is a deep copy. Have to do this because Matrix::InplaceTranspose is not implemented.
    if (recurrentOp == L"lstm") // rnnReLU and rnnTanh have one gate so reordering is moot.
        InplaceAdjustGateOrder(W0, layerOutputSize);
    //printf("\n---------W0 rearranged--------\n");
    //W0.Print();
    //printf("---------------------\n");
    return W0;
}

Matrix<float> CNTKToONNXHelper::GetBiasMatFromOrnnBigW(Matrix<float>& Wbig, size_t offset,
    size_t hiddenSize, size_t numGates, wstring recurrentOp)
{
    Matrix<float> b(1, 2 * hiddenSize*numGates, -1);

    Matrix<float> b1(-1), b2(-1);
    b1.SetValue(Wbig.ColumnSlice(offset, hiddenSize*numGates));
    /*printf("\n---------b1--------\n");
    b1.Print();
    printf("\n---------------------\n");*/
    auto nextoffset = offset + hiddenSize * numGates; // Note that 'offset' still must be updated outside. Not returning offset back, say through a tuple, because 
                                                      // that removes return value optimization (RVO) on the output argument of this method.
    b2.SetValue(Wbig.ColumnSlice(nextoffset, hiddenSize*numGates));
    /*printf("\n---------b2--------\n");
    b2.Print();
    printf("\n---------------------\n");*/
    // Creating bias vector b as [W_b, R_b]. Creating these values as done in
    // optimized_rnnstack_converter.py. W_bias is b1 + b2. R_bias is just zeros.
    b1.AssignSumOf(b1, b2);
    /*printf("\n---------b1 + b2--------\n");
    b1.Print();
    printf("\n---------------------\n");*/
    if (recurrentOp == L"lstm") // rnnReLU and rnnTanh have only one gates so reordering is moot.
        InplaceAdjustGateOrder(b1, hiddenSize);
    /*printf("\n---------b1 + b2 - rearranged--------\n");
    b1.Print();
    printf("\n---------------------\n");*/
    b.SetColumnSlice(b1, 0, hiddenSize * numGates);
    /*printf("\n---------b with only bias for W (first half)--------\n");
    b.Print();
    printf("\n---------------------\n");*/
    b.SetColumnSlice(Matrix<float>::Zeros(1, hiddenSize * numGates, -1), hiddenSize * numGates, hiddenSize * numGates);
    /*printf("\n---------Final b (second half zeros)--------\n");
    b.Print();
    printf("\n---------------------\n");*/

    return b;
}

void CNTKToONNXHelper::InplaceAdjustGateOrder(Matrix<float>& W, size_t hiddenSize)
{
    // REVIEW sptiwari: Written just for LSTM. Assumes numGates = 4. GRU to be included later.

    size_t offset(0);

    Matrix<float> Wi(-1), Wf(-1), Wc(-1), Wo(-1);
    Wi.SetValue(W.ColumnSlice(offset, hiddenSize));
    offset += hiddenSize;
    Wf.SetValue(W.ColumnSlice(offset, hiddenSize));
    offset += hiddenSize;
    Wc.SetValue(W.ColumnSlice(offset, hiddenSize));
    offset += hiddenSize;
    Wo.SetValue(W.ColumnSlice(offset, hiddenSize));

    offset = 0;
    W.SetColumnSlice(Wi, offset, hiddenSize);
    offset += hiddenSize;
    W.SetColumnSlice(Wo, offset, hiddenSize);
    offset += hiddenSize;
    W.SetColumnSlice(Wf, offset, hiddenSize);
    offset += hiddenSize;
    W.SetColumnSlice(Wc, offset, hiddenSize);
}

std::vector<NDArrayViewPtr> CNTKToONNXHelper::ToRnnWeightPerLayerOnnxFormat(std::vector<Matrix<float> >& W, size_t numLayers,
    size_t numDirections, size_t numGates, size_t hiddenSize, size_t inputSize, bool updateInputSizeWithEachLayer)
{
    std::vector<NDArrayViewPtr> Wonnx;
    // First layer input size is inputSize. Other layers' input size is numDirections*hiddenSize.
    size_t layerInputSize(inputSize);
    for (size_t i = 0; i < numLayers; ++i)
    {
        // Here we create a currLayerWeightMatrix which has 3D tensor in a 2D matrix data buffer.
        // The format is [Plane 1 Plane2]. This is the buffer format needed to pass in to NDArrayView
        // for creating a 3D tensor as needed by ONNX LSTM.
        Matrix<float> currLayerWeightMatrix(hiddenSize * numGates, layerInputSize * numDirections, -1);
        size_t offset = 0;
        for (size_t j = 0; j < numDirections; ++j)
        {
            Matrix<float> temp = W[i*numDirections + j].Transpose(); // Have to do this because Matrix::InplaceTranspose is not implemented.
            currLayerWeightMatrix.SetColumnSlice(temp, offset, layerInputSize);
            offset += layerInputSize;
        }
        NDArrayView currLayerWeightNDArray(::CNTK::DataType::Float, NDShape({ numDirections, hiddenSize*numGates, layerInputSize }),
            (void*)currLayerWeightMatrix.Data(), currLayerWeightMatrix.BufferSize(), DeviceDescriptor::CPUDevice());
        Wonnx.push_back(currLayerWeightNDArray.DeepClone(DeviceDescriptor::CPUDevice()));
        // Wonnx.push_back(std::make_shared<NDArrayView>(currLayerWeightNDArray));
        /*Wonnx.push_back(MakeSharedObject<NDArrayView>(::CNTK::DataType::Float, NDShape({ numDirections, hiddenSize*numGates, layerInputSize }),
            (void*)currLayerWeightMatrix.Data(), currLayerWeightMatrix.BufferSize(), DeviceDescriptor::CPUDevice()));*/

        if (updateInputSizeWithEachLayer)
        {
            // Except for first layer (so starting from second layer), the layer input sizes are 
            // hiddenSize for one directional, and 2*hiddenSize for bidirectional. This is needed
            // for W matrix which is based on inputSize, but not the H (sometimes called R) matrix, 
            // which is just based on hiddenSize which does not change.
            layerInputSize = hiddenSize * numDirections;
        }
    }
    return Wonnx;
}

std::vector<NDArrayViewPtr> CNTKToONNXHelper::ToRnnBiasPerLayerOnnxFormat(std::vector<Matrix<float> >& B, size_t numLayers,
    size_t numDirections, size_t hiddenSize, size_t numGates)
{
    std::vector<NDArrayViewPtr> Bonnx;
    for (size_t i = 0; i < numLayers; ++i)
    {
        // Here we create a currLayerWeightMatrix which has 3D tensor in a 2D matrix data buffer.
        // The format is [Plane 1 Plane2]. This is the buffer format needed to pass in to NDArrayView
        // for creating a 3D tensor as needed by ONNX LSTM.
        Matrix<float> currLayerBiasMatrix(2 * hiddenSize * numGates, numDirections, -1);
        size_t offset = 0;
        for (size_t j = 0; j < numDirections; ++j)
        {
            Matrix<float> temp = B[i*numDirections + j].Transpose(); // Have to do this because Matrix::InplaceTranspose is not implemented.
            currLayerBiasMatrix.SetColumnSlice(temp, offset, 1);
            ++offset;
        }
        // auto finalBiasMatrix = currLayerBiasMatrix.Transpose(); // Deep copy
        /*printf("\n currLayerBiasMatrix Before--------------\n");
        currLayerBiasMatrix.Print();
        printf("\n--------------------------------\n");*/
        NDArrayView currLayerBiasNDArray(::CNTK::DataType::Float, NDShape({ numDirections, 2 * hiddenSize*numGates }),
            (void*)currLayerBiasMatrix.Data(), currLayerBiasMatrix.BufferSize(), DeviceDescriptor::CPUDevice());
        Bonnx.push_back(currLayerBiasNDArray.DeepClone(DeviceDescriptor::CPUDevice()));
        //Bonnx.push_back(MakeSharedObject<NDArrayView>(::CNTK::DataType::Float, NDShape({ numDirections, 2 * hiddenSize*numGates }),
        //    (void*)finalBiasMatrix.Data(), finalBiasMatrix.BufferSize(), DeviceDescriptor::CPUDevice()));
        /*printf("\n currLayerBiasMatrix After--------------\n");
        PrintNDArrayView(Bonnx[Bonnx.size() - 1]);
        printf("\n--------------------------------\n");*/
    }
    return Bonnx;
}

void CNTKToONNXHelper::CreateRecurrentWeightONNXNodes(ONNXIR::Graph* graph, std::unordered_map<Variable, ONNXIR::Node*>& variableNodes,
    const Variable& Wcombined, std::vector<ONNXIR::NodeArg>& inputs, NDArrayViewPtr W, string WArgName)
{
    auto WArgType = ToTypeProto(W->Shape(), false, false, false); // Last arg is false because we don't want shape reversal here.
    UpdateONNXType(Wcombined.GetDataType(), WArgType); // NOTE: Only float type supported.
    ONNXIR::NodeArg WArg(WArgName, &WArgType);
    inputs.push_back(WArg);

    std::vector<ONNXIR::NodeArg> varInputs;
    std::vector<ONNXIR::NodeArg> varOutputs;

    varOutputs.push_back({ WArg });
    ONNXIR::Node* variableNode = graph->AddNode(WArgName, "Constant", "", varInputs, varOutputs);
    onnx::TensorProto dstTensor;
    /*printf("\n Creating weight and bias ONNX nodes After--------------\n");
    PrintNDArrayView(W);
    printf("--------------------------------\n");*/
    CopyTensor(W, dstTensor, &WArgType);
    variableNode->AddAttribute("value", dstTensor);
    variableNodes.emplace(Wcombined, variableNode); // REVIEW: Wcombined is same for all layers and all W and H. Is this correct or will it create trouble creating the graph?
}

ONNXIR::NodeArg CNTKToONNXHelper::LSTMOutputShapeAdapter(ONNXIR::NodeArg& inputArg, onnx::TypeProto& inputArgType, ONNXIR::Graph* graph,
    size_t numDirections, size_t hiddenSize, CNTK::DataType outputType, string adapterBasename)
{
    // This adapter changes input format (this is output of previous layer) from
    // [S, numDirections, B, hiddenSize] --> Output format (input to new LSTM layer) [S, B, numDirections*hiddenSize]

    // Transpose 2nd and 3rd axes, i.e. [S, numDirections, B, hiddenSize] --> [S, B, numDirections, hiddenSize]
    TensorShapeProto inputShape = inputArgType.mutable_tensor_type()->shape();
    onnx::TypeProto transposeOutputArgType;
    int inputRank = inputShape.dim_size();
    std::vector<int64_t> x(inputRank);
    std::iota(x.begin(), x.end(), 0);
    std::swap(x[inputRank - 2], x[inputRank - 3]); // swap (last but one) and (last but two)
    for (int index = 0; index < inputRank; index++)
    {
        if (inputShape.dim(static_cast<int>(x[index])).has_dim_param()) // For sequence axis, which is a dynamic axis.
            transposeOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param(inputShape.dim(static_cast<int>(x[index])).dim_param());
        else
            transposeOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(inputShape.dim(static_cast<int>(x[index])).dim_value());
    }
    UpdateONNXType(outputType, transposeOutputArgType);
    ONNXIR::NodeArg transposeOutputArg(adapterBasename + "_Transpose_Output", &transposeOutputArgType);
    auto transposeNode = graph->AddNode(adapterBasename + "_Transpose", "Transpose", "", { inputArg }, { transposeOutputArg });
    transposeNode->AddAttribute("perm", x);

    // Reshape to combine last two axes, i.e. [S, B, numDirections, hiddenSize] --> [S, B, numDirections*hiddenSize]
    TensorShapeProto lastShape = transposeOutputArgType.mutable_tensor_type()->shape();
    int lastShapeRank = lastShape.dim_size();
    if (lastShapeRank != 4)
        LogicError("Rank of the LSTM output from previous layer must be 4.");
    if (lastShape.dim(2).has_dim_param() || lastShape.dim(3).has_dim_param())
        LogicError("Sequence axis cannot be amongst the last two axis. It must be the first one.");
    onnx::TypeProto reshapeOutputArgType;
    if (lastShape.dim(0).has_dim_param())
        reshapeOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param(lastShape.dim(0).dim_param());
    else
        reshapeOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(lastShape.dim(0).dim_value());
    if (lastShape.dim(1).has_dim_param())
        reshapeOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_param(lastShape.dim(1).dim_param());
    else
        reshapeOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(lastShape.dim(1).dim_value());
    reshapeOutputArgType.mutable_tensor_type()->mutable_shape()->add_dim()->set_dim_value(lastShape.dim(2).dim_value()*lastShape.dim(3).dim_value());
    UpdateONNXType(outputType, reshapeOutputArgType);
    ONNXIR::NodeArg reshapeOutputArg(adapterBasename + "_Reshape_Output", &reshapeOutputArgType);
    auto reshapeNode = graph->AddNode(adapterBasename + "_Reshape", "Reshape", "", { transposeOutputArg }, { reshapeOutputArg });
    std::vector<int64_t> shape({ 0, 0, -1 });
    reshapeNode->AddAttribute("shape", shape);

    return reshapeOutputArg;
}
