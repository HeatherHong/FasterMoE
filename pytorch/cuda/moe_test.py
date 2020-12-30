from moe import MOELayer
import torch
import time


def perf():
    batch_size = 128
    in_feat = 1024
    out_feat = 4096
    num_expert = 4

    inp = torch.rand(batch_size, in_feat).cuda()
    gate = torch.randint(low=0, high=num_expert, size=(batch_size, ), requires_grad=False).int().cuda()

    moe = MOELayer(num_expert, in_feat, out_feat).cuda()

    o = moe(inp, gate)

    n_runs = 16
    tott = 0.
    for i in range(n_runs):
        gate = torch.randint(low=0, high=num_expert, size=(batch_size, ), requires_grad=False).int().cuda()
        ts = time.time()
        o = moe(inp, gate)
        te = time.time()
        tott += te - ts

    gflops = 2e-9 * n_runs * in_feat * out_feat * batch_size
    print('Mean time {:.3f} ms, {:.3f} GFLOPs'.format(tott * 1e3 / n_runs, gflops))


if __name__ == '__main__':
    perf()