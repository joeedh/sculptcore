  template <class Ctx>
float3 &CommandExecutor::nbrNo(Ctx &ctx, int v)
  {
    return ctx.node.data->m->v.no[v];
  }

  template <class Ctx>
const float3 *CommandExecutor::liveVertNoPtr(Ctx &ctx, int v) const
  {
    return ctx.m ? &ctx.m->v.no[v] : nullptr;
  }

  template <class AccMode>
  BasicVertexIter<AccMode> CommandExecutor::makeVertexIter(spatial::SpatialNode &node)
  {
    return BasicVertexIter<AccMode>(node, *this, ctx.dispVec, ctx.dispGen, ctx.strokeGen);
  }

  template <class AccMode>
  bool CommandExecutor::createCommandSwitch(SculptBrushes brushType,
                                  bool csrNeighbors,
                                  Brush *brushOrNull,
                                  brush_command &def)
  {
    if (command::createBuiltinBrush<CommandExecutor, CsrNbr, LiveDiskNbr, AccMode>(
            int(brushType), csrNeighbors, def))
    {
      return true;
    }
    // Extra (out-of-repo) kernels dispatch through the generated registry;
    // a no-op fallback compiles in when no extra kernel dirs are configured.
    return brushOrNull &&
           command::createExtraBrush<CommandExecutor, CsrNbr, LiveDiskNbr, AccMode>(
               int(brushType), csrNeighbors, *brushOrNull, def);
  }

  template <class AccMode>
  void CommandExecutor::createCommandImpl(SculptBrushes brushType, brush_command &def)
  {
    if (createCommandSwitch<AccMode>(
            brushType, effectiveNeighborMode() == NeighborMode::Csr, brush, def))
    {
      return;
    }
    printf("Unknown brush type %d\n", static_cast<int>(brushType));
    abort();
  }
