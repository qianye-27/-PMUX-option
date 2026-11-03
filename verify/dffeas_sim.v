// Minimal behavioural model of the Cyclone IV dffeas primitive, used only for
// equivalence checking (post-ABC netlists still contain dffeas instances).
// Mirrors the instance ports produced by +/intel/common/ff_map.v:
//   prn=1, sclr=0, sload=0, aload=0, asdata=0
module dffeas (
  output reg q,
  input d, clk, clrn, prn, ena,
  input asdata, aload, sclr, sload
);
  parameter power_up = "dontcare";
  parameter is_wysiwyg = "false";

  initial begin
    if (power_up == "high")
      q = 1'b1;
    else if (power_up == "low")
      q = 1'b0;
  end

  always @(posedge clk or negedge clrn)
    if (!clrn)
      q <= 1'b0;
    else if (ena)
      q <= d;
endmodule
