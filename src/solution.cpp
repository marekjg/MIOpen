/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/

#include <miopen/solution.hpp>

#include <miopen/any_solver.hpp>
#include <miopen/check_numerics.hpp>
#include <miopen/conv/data_invoke_params.hpp>
#include <miopen/conv/wrw_invoke_params.hpp>

#include <nlohmann/json.hpp>

#include <boost/hof/match.hpp>
#include "miopen/fusion/problem_description.hpp"
#include "miopen/fusion/context.hpp"

namespace miopen::debug {
// Todo: This should be updated when a separate driver command is implemented
void LogCmdConvolution(const miopen::TensorDescriptor& x,
                       const miopen::TensorDescriptor& w,
                       const miopen::ConvolutionDescriptor& conv,
                       const miopen::TensorDescriptor& y,
                       miopenProblemDirection_t dir,
                       std::optional<uint64_t> solver_id);
} // namespace miopen::debug

namespace miopen {

void Solution::Run(Handle& handle,
                   const std::unordered_map<miopenTensorArgumentId_t, RunInput>& inputs,
                   Data_t workspace,
                   std::size_t workspace_size)
{
    if(workspace_size < workspace_required)
    {
        MIOPEN_THROW(miopenStatusBadParm,
                     GetSolver().ToString() + " requires at least " +
                         std::to_string(workspace_required) + " workspace, while " +
                         std::to_string(workspace_size) + " was provided");
    }

    boost::apply_visitor(
        boost::hof::match(
            [&](const Problem& problem_) {
                boost::apply_visitor(
                    boost::hof::match(
                        [&](const ConvolutionDescriptor& op_desc) {
                            RunImpl(handle, inputs, workspace, workspace_size, op_desc);
                        },
                        [&](const ActivationDescriptor& /*op_desc*/) {
                            MIOPEN_THROW(miopenStatusNotImplemented);
                        },
                        [&](const BiasDescriptor& /*op_desc*/) {
                            MIOPEN_THROW(miopenStatusNotImplemented);
                        }),
                    problem_.GetOperatorDescriptor());
            },
            [&](const FusedProblem& problem_) {
                RunImpl(handle, inputs, workspace, workspace_size, problem_);
            }),
        problem.item);
}

void Solution::LogDriverCommand() const
{
    boost::apply_visitor([&](const auto& problem_) { LogDriverCommand(problem_); }, problem.item);
}

void Solution::LogDriverCommand(const ConvolutionDescriptor& desc) const
{
    auto problem_ = boost::get<const Problem&>(problem.item);
    const auto& x_desc =
        problem_.GetTensorDescriptorChecked(miopenTensorConvolutionX, "miopenTensorConvolutionX");
    const auto& w_desc =
        problem_.GetTensorDescriptorChecked(miopenTensorConvolutionW, "miopenTensorConvolutionW");
    const auto& y_desc =
        problem_.GetTensorDescriptorChecked(miopenTensorConvolutionY, "miopenTensorConvolutionY");
    miopen::debug::LogCmdConvolution(
        x_desc, w_desc, desc, y_desc, problem_.GetDirection(), solver.Value());
}

void Solution::LogDriverCommand(const ActivationDescriptor& desc) const
{
    std::ignore = desc;
    boost::get<Problem>(problem.item).LogDriverCommand();
    /// \todo: when possible, add some command for reproducing a specific case rather than the whole
    /// problem
}

void Solution::LogDriverCommand(const Problem& problem_) const
{
    boost::apply_visitor(
        boost::hof::match([&](const BiasDescriptor&) { /* \todo: think on how to log bias */ },
                          [&](const auto& op_desc) { LogDriverCommand(op_desc); }),
        problem_.GetOperatorDescriptor());
}

void Solution::LogDriverCommand(const FusedProblem& problem_) const
{
    std::ignore = problem_;
    /// \todo: add logging of some command to reproduce current solution or at least problem
}

void Solution::RunImpl(Handle& handle,
                       const std::unordered_map<miopenTensorArgumentId_t, RunInput>& inputs,
                       Data_t workspace,
                       std::size_t workspace_size,
                       const ConvolutionDescriptor& conv_desc)
{
    const auto& problem_casted = boost::get<const Problem&>(problem.item);

    const auto get_input_checked = [&](auto name, const std::string& name_str) {
        const auto& found = inputs.find(name);
        if(found == inputs.end())
        {
            MIOPEN_THROW(miopenStatusInvalidValue,
                         "Problem is missing " + name_str + " tensor descriptor.");
        }
        auto ret = found->second;
        if(!ret.descriptor.has_value())
            ret.descriptor = problem_casted.GetTensorDescriptorChecked(name, name_str);
        return ret;
    };

    auto x       = get_input_checked(miopenTensorConvolutionX, "miopenTensorConvolutionX");
    const auto w = get_input_checked(miopenTensorConvolutionW, "miopenTensorConvolutionW");
    auto y       = get_input_checked(miopenTensorConvolutionY, "miopenTensorConvolutionY");

    const auto problem_ =
        conv_desc.mode == miopenTranspose ? Transpose(problem_casted, &x, w, &y) : problem_casted;

    if(problem_.GetDirection() == miopenProblemDirectionBackward &&
       y.descriptor->GetLengths()[1] != w.descriptor->GetLengths()[0])
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }

    if(miopen::CheckNumericsEnabled())
    {
        if(problem_.GetDirection() != miopenProblemDirectionBackward)
            miopen::checkNumericsInput(handle, *x.descriptor, x.buffer);
        if(problem_.GetDirection() != miopenProblemDirectionBackwardWeights)
            miopen::checkNumericsInput(handle, *w.descriptor, w.buffer);
        if(problem_.GetDirection() != miopenProblemDirectionForward)
            miopen::checkNumericsInput(handle, *y.descriptor, y.buffer);
    }

    const auto conv_problem = problem_.AsConvolution();

    Problem::ValidateGroupCount(*x.descriptor, *w.descriptor, conv_problem.GetConv());

    const auto invoke_ctx = [&]() -> AnyInvokeParams {
        switch(problem_.GetDirection())
        {
        case miopenProblemDirectionForward:
            return conv::DataInvokeParams(
                {*x.descriptor, x.buffer, *w.descriptor, w.buffer, *y.descriptor, y.buffer},
                workspace,
                workspace_size,
                conv_problem.GetConv().attribute.gfx90aFp16alt.GetFwd());
        case miopenProblemDirectionBackward:
            return conv::DataInvokeParams(
                {*y.descriptor, y.buffer, *w.descriptor, w.buffer, *x.descriptor, x.buffer},
                workspace,
                workspace_size,
                conv_problem.GetConv().attribute.gfx90aFp16alt.GetBwd());
        case miopenProblemDirectionBackwardWeights:
            return conv::WrWInvokeParams{
                {*y.descriptor, y.buffer, *x.descriptor, x.buffer, *w.descriptor, w.buffer},
                workspace,
                workspace_size,
                conv_problem.GetConv().attribute.gfx90aFp16alt.GetWrW()};
        default: MIOPEN_THROW(miopenStatusNotImplemented);
        }
    }();

    const auto net_cfg       = conv_problem.MakeNetworkConfig();
    const auto found_invoker = handle.GetInvoker(net_cfg, GetSolver());

    const auto checkNumericsOutput_ = [&]() {
        if(miopen::CheckNumericsEnabled())
        {
            if(problem_.GetDirection() == miopenProblemDirectionBackward)
                miopen::checkNumericsOutput(handle, *x.descriptor, x.buffer);
            if(problem_.GetDirection() == miopenProblemDirectionBackwardWeights)
                miopen::checkNumericsOutput(handle, *w.descriptor, w.buffer);
            if(problem_.GetDirection() == miopenProblemDirectionForward)
                miopen::checkNumericsOutput(handle, *y.descriptor, y.buffer);
        }
    };

    if(found_invoker)
    {
        (*found_invoker)(handle, invoke_ctx);
        checkNumericsOutput_();
        return;
    }

    auto conv_ctx = ExecutionContext{&handle};
    conv_problem.SetupFloats(conv_ctx);

    decltype(auto) db        = GetDb(conv_ctx);
    const auto conv_solution = GetSolver().GetSolver().FindSolution(
        conv_ctx, conv_problem, db, invoke_ctx, perf_cfg.value_or(""));
    decltype(auto) invoker =
        handle.PrepareInvoker(*conv_solution.invoker_factory, conv_solution.construction_params);
    handle.RegisterInvoker(invoker, net_cfg, GetSolver().ToString());
    invoker(handle, invoke_ctx);
    checkNumericsOutput_();
}

void Solution::RunImpl(Handle& handle,
                       const std::unordered_map<miopenTensorArgumentId_t, RunInput>& inputs,
                       Data_t /*workspace*/,
                       std::size_t /*workspace_size*/,
                       const FusedProblem& problem_)
{
    const auto buffer_getter = [&](auto id, auto&& descriptor) {
        const auto found = inputs.find(id);
        if(found == inputs.end())
            MIOPEN_THROW(miopenStatusInvalidValue,
                         "Problem is missing " + std::to_string(id) + " tensor descriptor.");
        if(found->second.descriptor.has_value() && *found->second.descriptor != descriptor)
            MIOPEN_THROW(miopenStatusNotImplemented,
                         "Providing new descriptors for a fused solution is not supported.");
        return found->second.buffer;
    };

    OperatorArgs op_args;
    const auto invoke_params = problem_.MakeInvokeParams(buffer_getter, op_args);

    const auto plan           = problem_.AsFusionPlan();
    const auto fusion_problem = FusionDescription{&plan};
    const auto net_cfg        = fusion_problem.MakeNetworkConfig();

    const auto found_invoker = handle.GetInvoker(net_cfg, GetSolver());

    if(found_invoker)
    {
        (*found_invoker)(handle, invoke_params);
        return;
    }

    const auto ctx      = FusionContext{handle};
    const auto solution = MakeFusedSolution(ctx, solver, perf_cfg, fusion_problem, invoke_params);
    decltype(auto) invoker =
        handle.PrepareInvoker(*solution.invoker_factory, solution.construction_params);
    handle.RegisterInvoker(invoker, net_cfg, GetSolver().ToString());
    invoker(handle, invoke_params);
}

Problem Solution::Transpose(const Problem& problem, RunInput* x, const RunInput& w, RunInput* y)
{
    auto transposed = problem.MakeTransposed();

    std::swap(*x, *y);

    if(x->descriptor)
        transposed.RegisterTensorDescriptor(miopenTensorConvolutionX, *x->descriptor);
    if(w.descriptor)
        transposed.RegisterTensorDescriptor(miopenTensorConvolutionW, *w.descriptor);
    if(y->descriptor)
        transposed.RegisterTensorDescriptor(miopenTensorConvolutionY, *y->descriptor);

    return transposed;
}

void to_json(nlohmann::json& json, const Solution::SerializationMetadata& metadata)
{
    json = nlohmann::json{
        {"validation", metadata.validation_number},
        {"version", metadata.version},
    };
}
void from_json(const nlohmann::json& json, Solution::SerializationMetadata& metadata)
{
    json.at("validation").get_to(metadata.validation_number);
    json.at("version").get_to(metadata.version);
}

void to_json(nlohmann::json& json, const Solution& solution)
{
    json = nlohmann::json{
        {"header", Solution::SerializationMetadata::Current()},
        {"time", solution.time},
        {"workspace", solution.workspace_required},
        {"solver", solution.solver.ToString()},
        {"problem", solution.problem},
    };

    if(solution.perf_cfg.has_value())
        json["perf_cfg"] = *solution.perf_cfg;
}

void from_json(const nlohmann::json& json, Solution& solution)
{
    {
        const auto header = json.at("header").get<Solution::SerializationMetadata>();
        constexpr const auto check_header = Solution::SerializationMetadata::Current();

        if(header.validation_number != check_header.validation_number)
        {
            MIOPEN_THROW(miopenStatusInvalidValue,
                         "Invalid buffer has been passed to the solution deserialization.");
        }
        if(header.version != check_header.version)
        {
            MIOPEN_THROW(
                miopenStatusVersionMismatch,
                "Data from wrong version has been passed to the solution deserialization.");
        }
    }

    json.at("time").get_to(solution.time);
    json.at("workspace").get_to(solution.workspace_required);
    solution.solver = json.at("solver").get<std::string>();
    json.at("problem").get_to(solution.problem);

    const auto perf_cfg_json = json.find("perf_cfg");
    solution.perf_cfg        = perf_cfg_json != json.end()
                                   ? std::optional{perf_cfg_json->get<std::string>()}
                                   : std::nullopt;
}
} // namespace miopen
