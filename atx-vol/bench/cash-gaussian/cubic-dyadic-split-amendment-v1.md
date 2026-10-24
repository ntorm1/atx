# Bounded split-product amendment

This corrects a resource implementation assumption in the prospective exact
dyadic-zero proof; the exact identity,16 rational unit fixtures and all pricing
gates remain unchanged. It introduces no full-tail or matrix experiment.

The installed Boost implementation can allocate variable-precision Karatsuba
scratch even when the public integer backend has fixed precision and a void
allocator. The first attempted compile guard rejects the unsplit product:
this clang-cl build uses32-bit limbs, so an operand below2100 bits occupies up
to66 limbs, exceeding the installed40-limb threshold. The failed build and
unsplit source remain in `E/calendar/cash-cubic-dyadic-zero-v1/`; no successful
numerical result from that implementation is claimed.

Before multiplication, split the nonnegative width H and the absolute slope
sum M at B=1024 bits. With mask `2^B-1`, form H0/H1 and M0/M1, then compute

```
P00 = H0*M0; P01 = H0*M1; P10 = H1*M0; P11 = H1*M1
H*M = P00 + ((P01+P10) << B) + (P11 << (2*B)).
```

Each factor has at most1075 magnitude bits, hence at most34 actual32-bit limbs.
The source must compile-time check that bound against the installed backend's
actual limb width and Karatsuba threshold. A future incompatible backend fails
this isolated research build; no macro changes, modified Boost source or
unbounded-allocation fallback may bypass the guard. The result still uses the
same fixed checked8192-bit integer type with expression templates disabled.
All valid intermediate values stay below4200 magnitude bits.

All four half-products, bit masks, shifts and recombination sums are explicit.
Apply the slope-sum sign only after completing nonnegative reconstruction.
The value-difference term likewise shifts its nonnegative magnitude before
restoring its sign. No bitwise operation acts on a negative signed integer.

The full proof now has at most6 decoded operands,5 products (four half-products
and the scalar factor3),8 sums/subtractions,13 shifts,2 masks and32 magnitude
bit scans. Counts refer to these logical integer operations, not individual
machine limb instructions. Every resource check precedes its attempted
operation; partial failures retain actual work and return no partial proof.

The workspace owns six inputs, the original seven intermediate objects and
eight split-product objects:21 complete fixed integers. Reserve its actual
`sizeof`, eight additional complete integer temporaries and4096 bytes for
bounded strings/metadata before integer construction. This is a construction
reservation, excluding allocator metadata and not a measured resident-memory
claim. Checked operations and the no-Karatsuba compile guard remain independent
of this accounting. The original64KiB per-proof ceiling is retained.

Keep the previously captured six runtime-red unit/owner controls and the
separate failed compile-guard receipt. The independent Fraction fixtures and
their expected exact numerators are not regenerated to accommodate this
implementation. Recheck both input signs, maximum/subnormal exponents, proof
budgets and the exact-nonzero rounded-zero adversary before fresh source review.
