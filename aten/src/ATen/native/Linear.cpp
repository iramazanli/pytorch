#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/native/xnnpack/Engine.h>
#include <ATen/WrapDimUtilsMulti.h>
#include <c10/macros/Macros.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <vector>

namespace at { namespace native {

Tensor linear(const Tensor& input, const Tensor& weight, const Tensor& bias) {
  if (input.is_mkldnn()) {
    return at::mkldnn_linear(input, weight, bias);
  }
#if defined(C10_MOBILE)
  if (xnnpack::use_linear(input, weight, bias)) {
    return xnnpack::linear(input, weight, bias);
  }
#endif
  if (input.dim() == 2 && bias.defined()) {
    // Fused op is marginally faster.
    return at::addmm(bias, input, weight.t());
  }
  auto output = at::matmul(input, weight.t());
  if (bias.defined()) {
    output.add_(bias);
  }
  return output;
}

// sumproduct_pair computes `(left*right).sum(sumdims)` by means of permutation and
// batch matrix multiplication
// its main purpose is to provide a pairwise reduction for einsum
static Tensor sumproduct_pair(const Tensor& left_, const Tensor& right_, IntArrayRef sum_dims_, bool keepdim) {
  // assumes that tensors have been pre-unsqueezed (so that all dimensions match - after broadcasting)
  // but makes no other assumptions on the order of dimensions
  TORCH_CHECK(left_.dim()==right_.dim(), "number of dimensions must match");
  if (sum_dims_.size() == 0)
    return at::mul(left_, right_);
  int64_t dim = left_.dim();
  auto sum_dims = at::dim_list_to_bitset(sum_dims_, dim);
  // dimensions that will be part of the output (i.e. not summed over) in three vectors
  // dims in lro appear in left, right and output, similarly lo: left and output, ro: right and output
  // also the sizes are kept track of for reshaping
  std::vector<int64_t> lro, lo, ro;
  int64_t lro_size = 1, lo_size = 1, ro_size = 1, sum_size = 1;
  Tensor left = left_;
  Tensor right = right_;
  for (int64_t i = 0; i < dim; i++) {
    auto sl = left.size(i)>1;
    auto sr = right.size(i)>1;
    if (sum_dims[i]) { // first dimensions that will be summed over after multiplication
      if (sl && sr) {  // dimensions nontrivially in both left and right must be of the same size
        TORCH_CHECK(left.size(i)==right.size(i), "non-broadcast dimensions must match");
        sum_size *= left.size(i);
      } else if (sl) { // if it is only in one of left and right, we can sum right away
        left = left.sum(i, true);
      } else if (sr) {
        right = right.sum(i, true);
      }
    } else if (sl && sr) { // now deal with dimensions  dimensions that will be in the output
      // dimensions nontrivially in both left and right must be of the same size
      TORCH_CHECK(left.size(i)==right.size(i), "non-broadcast dimensions must match");
      lro.push_back(i);
      lro_size *= left.size(i);
    } else if (sl) { // keep track of dimensions appearing only once
      lo.push_back(i);
      lo_size *= left.size(i);
    } else {
      ro.push_back(i);
      ro_size *= right.size(i);
    }
  }
  // we now work with the following permutations / shapes.
  // the pipeline is permute inputs -> reshape inputs -> batch matrix mul -> reshape(view) output -> permute output
  // output: "lro, lo, 1-for-summed-dims, ro" with orgiginal shape dimensions
  // left:   "lro, lo, summed" permuted with lpermutation and the three flattened
  // right:  "lro, summed, ro" permuted with rpermutation and the three flattened
  // then the permuted output is a view of bmm(left, right)
  // finally, opermutation reverts the permutation to the original order of dimensions
  std::vector<int64_t> out_size;
  for (auto& d : lro) out_size.push_back(left.size(d));
  for (auto& d : lo) out_size.push_back(left.size(d));
  for (auto& d : sum_dims_) { out_size.push_back(1); (void)(d); }; // avoid warining about not using d
  for (auto& d : ro) out_size.push_back(right.size(d));

  std::vector<int64_t> lpermutation(lro);
  lpermutation.insert(lpermutation.end(), lo.begin(), lo.end());
  lpermutation.insert(lpermutation.end(), sum_dims_.begin(), sum_dims_.end());
  lpermutation.insert(lpermutation.end(), ro.begin(), ro.end());

  std::vector<int64_t> rpermutation(lro);
  rpermutation.insert(rpermutation.end(), sum_dims_.begin(), sum_dims_.end());
  rpermutation.insert(rpermutation.end(), ro.begin(), ro.end());
  rpermutation.insert(rpermutation.end(), lo.begin(), lo.end());

  std::vector<int64_t> opermutation(lro.size()+lo.size()+sum_dims_.size()+ro.size(), -1);
  {
  int64_t i = 0;

  for (auto it = lro.begin(); it != lro.end(); i++, it++) {
    opermutation[*it] = i;
  }
  for (auto it = lo.begin(); it != lo.end(); i++, it++) {
    opermutation[*it] = i;
  }
  for (auto it = sum_dims_.begin(); it != sum_dims_.end(); i++, it++) {
    opermutation[*it] = i;
  }
  for (auto it = ro.begin(); it != ro.end(); i++, it++) {
    opermutation[*it] = i;
  }
  }

  // now we can execute the operations above
  left = left.permute(lpermutation).reshape({lro_size, lo_size, sum_size});
  right = right.permute(rpermutation).reshape({lro_size, sum_size, ro_size});
  Tensor result = at::bmm(left, right);
  result = result.view(out_size).permute(opermutation);

  // finally squeeze summed dimensions if desired
  if (! keepdim) {
    auto sizes = result.sizes().vec();
    for (int i = dim-1; i>=0; i--) {
      if (sum_dims[i]) {
        sizes.erase(sizes.begin() + i);
      }
    }
    result = result.view(sizes);
  }
  return result;
}

Tensor einsum(std::string equation, TensorList operands) {
  TORCH_CHECK(!operands.empty(), "einsum() must provide at least one operand");

  // Find arrow (->) to split equation into lhs and rhs
  std::size_t arrow_pos = equation.find("->");
  
  // Extract labels for each operand
  std::istringstream lhs(equation.substr(0, arrow_pos));
  std::vector<std::string> operand_labels;
  while (lhs.good()) {
    std::string token;
    std::getline(lhs, token, ',');
    operand_labels.emplace_back(token);
  }
  
  TORCH_CHECK(
      operand_labels.size() == operands.size(),
      "einsum() the number of operands specified in the equation (",
      operand_labels.size(),
      ") does not match the number of operands provided (",
      operands.size(),
      ")");
  
  constexpr int total_labels = 'z' - 'a' + 1;
  std::vector<int> label_count(total_labels, 0);
  std::vector<std::size_t> label_last_operand(total_labels, -1);
  
  // Parse labels for each operand
  for (std::size_t i = 0; i < operands.size(); ++i) {
    Tensor operand = operands[i];
    std::string labels = operand_labels[i];
    int count = 0;
    for (char c : labels) {
      if (c != ' ') {
        TORCH_CHECK(
            c >= 'a' && c <= 'z',
            "einsum() subscripts must be in range [a, z] but found ",
            c);
        if (operand.size(count) > 1) {
          label_last_operand[c - 'a'] = i;
        }
        ++label_count[c - 'a'];
        ++count;
      }
    }
    TORCH_CHECK(
        count == operand.dim(),
        "einsum() the number of subscripts in the equation (",
        count,
        ") does not match the number of dimensions (",
        operand.dim(),
        ") for operand ",
        i);
  }

  // Parse output labels
  std::vector<int> label_out_index(total_labels, -1);
  std::vector<int> permuted_labels;
  int out_index = 0;
  if (arrow_pos == std::string::npos) {
    for (int label = 0; label < total_labels; ++label) {
      if (label_count[label] == 1) {
        label_out_index[label] = out_index++;
        permuted_labels.emplace_back(label);
      }
    }
  } else {
    std::string rhs = equation.substr(arrow_pos + 2);
    for (char c : rhs) {
      if (c != ' ') {
        TORCH_CHECK(
            c >= 'a' && c <= 'z',
            "einsum() subscripts must be in range [a, z] but found ",
            c);
        TORCH_CHECK(
            label_count[c - 'a'] > 0,
            "einsum() output subscript ",
            c,
            label_count[c - 'a'] == -1 
            ? " appears more than once in the output string"
            : " does not appear in the equation for any input operand");
        label_out_index[c - 'a'] = out_index++;
        label_count[c - 'a'] = -1;
        permuted_labels.emplace_back(c - 'a');
      }
    }
  }
  
  int out_size = out_index;

  // Parse contraction labels
  for (int label = 0; label < total_labels; ++label) {
    if (label_count[label] > 0 && label_out_index[label] == -1) {
      label_out_index[label] = out_index++;
      permuted_labels.emplace_back(label);
    }
  }

  // Permute operands to align dimensions (out_shape + contraction_shape) and
  // take diagonals for subscripts repeated in the same operand
  std::vector<Tensor> permuted_operands;
  for (std::size_t i = 0; i < operands.size(); ++i) {
    std::vector<int64_t> perm_shape(out_index, -1);
    std::vector<int64_t> label_dim(total_labels, -1);
    std::string labels = operand_labels[i];
    Tensor operand = operands[i];
    std::size_t j = 0;
    for (char c : labels) {
      if (c != ' ') {
        if (label_dim[c - 'a'] != -1) {
          // Repeated label, take diagonal
          int64_t dim = label_dim[c - 'a'];
          TORCH_CHECK(
              operand.size(j) == operand.size(dim),
              "einsum() subscript ",
              c,
              " is repeated for operand ",
              i,
              " but the sizes don't match, ",
              operand.size(j),
              " != ",
              operand.size(dim));
          operand = operand.diagonal(0, j, dim);
          operand.unsqueeze_(dim).transpose_(dim, -1).squeeze_(-1);
        } else {
          label_dim[c - 'a'] = j;
          perm_shape[label_out_index[c - 'a']] = j++;
        }
      }
    }
    for (int64_t& index : perm_shape) {
      if (index == -1) {
        operand = operand.unsqueeze(-1);
        index = j++;
      }
    }
    permuted_operands.push_back(operand.permute(perm_shape));
  }

  // Compute result
  Tensor result = permuted_operands[0];

  for (std::size_t i = 1; i < permuted_operands.size(); ++i) {
    result = result.mul(permuted_operands[i]);
  }
  
  if (out_size < out_index) {
    std::vector<int64_t> sum_dims(out_index - out_size);
    std::iota(sum_dims.begin(), sum_dims.end(), out_size);
    result = result.sum(sum_dims);
  }

  // if (permuted_operands.size() == 1 && out_size < out_index) {
  //   std::vector<int64_t> sum_dims(out_index - out_size);
  //   std::iota(sum_dims.begin(), sum_dims.end(), out_size);
  //   result = result.sum(sum_dims);
  // }

  // for (std::size_t i = 1; i < permuted_operands.size(); ++i) {
  //   std::vector<int64_t> sum_dims;
  //   for (int dim = out_size; dim < out_index; ++dim) {
  //     if (label_last_operand[permuted_labels[dim]] <= i) {
  //       sum_dims.emplace_back(dim);
  //     }
  //   }
  //   result = sumproduct_pair(
  //       result,
  //       permuted_operands[i],
  //       sum_dims,
  //       i < permuted_operands.size() - 1);
  // }

  return result;
}

// _trilinear computes a trilinear einstein sum with an unrolled dimension
// the result is `(i1.unsqueeze(expand1)*i2.unsqueeze(expand2)*i2.unsqueeze(expand3)).sum(sumdim)`
// the computation is unrolled in the unroll_dim dimension
// its main purpose is to unify the computations in bilinear and bilinear_backward
Tensor _trilinear(const Tensor& i1_, const Tensor& i2_, const Tensor& i3_,
                  IntArrayRef expand1_, IntArrayRef expand2_, IntArrayRef expand3_,
                  IntArrayRef sumdim_, int64_t unroll_dim) {
  int64_t total_dim = i1_.dim()+expand1_.size();
  TORCH_CHECK((unroll_dim >= 0) && (unroll_dim < total_dim), "unroll_dim must be in [0,", total_dim-1, "]");
  auto expand1 = at::dim_list_to_bitset(expand1_, total_dim);
  auto expand2 = at::dim_list_to_bitset(expand2_, total_dim);
  auto expand3 = at::dim_list_to_bitset(expand3_, total_dim);
  auto sumdim  = at::dim_list_to_bitset(sumdim_,  total_dim);
  Tensor i1 = i1_;
  Tensor i2 = i2_;
  Tensor i3 = i3_;
  std::vector<int64_t> output_size;
  std::vector<int64_t> sum_dims_12, sum_dims_23;
  int64_t unroll_size = -1;
  // asserts...
  for (int64_t i = 0; i < total_dim; i++) {
    int64_t s = 0;
    if (expand1[i]) {
      i1 = i1.unsqueeze(i);
    } else  {
      s = i1.size(i);
    }
    if (expand2[i]) {
      i2 = i2.unsqueeze(i);
    } else  {
      s = i2.size(i);
    }
    if (expand3[i]) {
      i3 = i3.unsqueeze(i);
      if (sumdim[i] && (i != unroll_dim))
        sum_dims_12.push_back(i);
    } else  {
      s = i3.size(i);
      if (sumdim[i] && (i != unroll_dim))
        sum_dims_23.push_back(i);
    }
    output_size.push_back(sumdim[i] ? 1 : s);
    if (i == unroll_dim)
      unroll_size = s;
  }
  int64_t slicemul1 = (expand1[unroll_dim] ? 0 : 1);
  int64_t slicemul2 = (expand2[unroll_dim] ? 0 : 1);
  int64_t slicemul3 = (expand3[unroll_dim] ? 0 : 1);

  auto output = at::zeros(output_size, i1.options());
  if (! sumdim[unroll_dim]) {
    for (int64_t k = 0; k < unroll_size; k++) {
      Tensor buf = at::native::sumproduct_pair(i1.narrow(unroll_dim, k * slicemul1, 1),
                                               i2.narrow(unroll_dim, k * slicemul2, 1),
                                               sum_dims_12, true);
      buf = at::native::sumproduct_pair(buf, i3.narrow(unroll_dim, k * slicemul3, 1), sum_dims_23, true);
      output.narrow(unroll_dim, k, 1).add_(buf);
    }
  }
  else {
    for (int64_t k = 0; k < unroll_size; k++) {
      Tensor buf = at::native::sumproduct_pair(i1.narrow(unroll_dim, k*slicemul1, 1),
                                               i2.narrow(unroll_dim, k*slicemul2, 1), sum_dims_12, true);
      buf = at::native::sumproduct_pair(buf, i3.narrow(unroll_dim, k*slicemul3, 1), sum_dims_23, true);
      output.add_(buf);
    }
  }
  for (int64_t i = output.dim()-1; i >= 0; i--)
    if (sumdim[i])
      output.squeeze_(i);
  return output;
}

Tensor bilinear(const Tensor& input1, const Tensor& input2, const Tensor& weight, const Tensor& bias) {
  TORCH_CHECK(input1.dim() == input2.dim(), "bilinear(): input dimensions do not match: got ", input1.dim(), " and ", input2.dim());
  for (int64_t i = 0; i < input1.dim() - 1; i++) {
    TORCH_CHECK(input1.size(i) == input2.size(i),
              "bilinear(): input batch dimensions do not match at dim ", i, ": got ", input1.size(i), " and ", input2.size(i));
  }
  TORCH_CHECK(input1.size(input1.dim() - 1) == weight.size(1),
            "bilinear(): input1 size does not match weight size: got ",
            input1.size(input1.dim() - 1), " but expected ", weight.size(1));
  TORCH_CHECK(input2.size(input2.dim() - 1) == weight.size(2),
            "bilinear(): input2 size does not match weight size: got ",
            input2.size(input2.dim() - 1), " but expected ", weight.size(2));
  TORCH_CHECK(!bias.defined() || bias.size(0) == weight.size(0),
            "bilinear(): bias size does not match weight size: got ",
            bias.size(0), " but expected ", weight.size(0));

  std::vector<int64_t> output_size;
  auto size1 = input1.sizes();
  output_size.insert(output_size.end(), size1.begin(), size1.end() - 1);
  output_size.push_back(weight.size(0));
  auto input1_flattened = input1.view({-1, input1.size(-1)});
  auto input2_flattened = input2.view({-1, input2.size(-1)});
  Tensor output = at::_trilinear(input1_flattened, weight, input2_flattened, {1,3}, {0}, {1,2}, {2,3}).reshape(output_size);
  if (bias.defined()) {
    output = output + bias;
  }
  return output;
}

// implements tensordot, a matrix-multiplication-like contraction, but the dimensions given
// in the two dimension lists
Tensor tensordot(const Tensor& input1, const Tensor& input2, IntArrayRef dims1, IntArrayRef dims2) {
  TORCH_CHECK(dims1.size() == dims2.size(), "both dimension lists should have same length");
  int64_t csize = 1;  // total size of the contracted dimensions
  Tensor t1 = input1;
  Tensor t2 = input2;
  for (size_t i = 0; i < dims1.size(); i++) {
    int s1 = input1.size(dims1[i]);
    int s2 = input2.size(dims2[i]);
    if (s2 == 1) { // broadcasted dimensions can be summed right away
      t1 = t1.sum(dims1[i], true);
    } else if (s1 == 1) {
      t2 = t2.sum(dims2[i], true);
    } else {
      TORCH_CHECK(s1 == s2, "contracted dimensions need to match, but first has size ", s1, " in dim ", dims1[i],
               " and second has size ", s2, " in dim ", dims2[i]);
      csize *= s1;
    }
  }

  auto cdims1 = at::dim_list_to_bitset(dims1, input1.dim());
  auto cdims2 = at::dim_list_to_bitset(dims2, input2.dim());
  std::vector<int64_t> p1, p2, rsizes;  // p1, p2: input permutations, rsizes: sizes of the result
  p1.reserve(input1.dim());
  p2.reserve(input2.dim());
  rsizes.reserve(input1.dim() + input2.dim() - (int64_t) dims1.size());
  int64_t size1 = 1; // number of non-contracted elements in input1
  int64_t size2 = 1; // number of non-contracted elements in input2

  // fill the permutations and compute sizes
  for (int64_t i = 0; i < input1.dim(); i++) {
    if (! cdims1[i]) {
      p1.emplace_back(i);
      size1 *= t1.size(i);
      rsizes.emplace_back(t1.size(i));
    }
  }
  for (size_t i = 0; i < dims1.size(); i++) {
    p1.emplace_back(dims1[i]);
  }
  for (size_t i = 0; i < dims2.size(); i++) {
    p2.emplace_back(dims2[i]);
  }
  for (int64_t i = 0; i < input2.dim(); i++) {
    if (! cdims2[i]) {
      p2.emplace_back(i);
      size2 *= t2.size(i);
      rsizes.emplace_back(t2.size(i));
    }
  }
  // permut and reshape for matrix multiplication
  t1 = t1.permute(p1).reshape({size1, csize});
  t2 = t2.permute(p2).reshape({csize, size2});
  // multiply and reshape to target size
  return at::mm(t1, t2).reshape(rsizes);
}

}}  // namespace at::native
