/*
 * Copyright (c) 2015, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */
package org.graalvm.compiler.phases.common;

import java.util.ArrayDeque;
import java.util.Deque;
import java.util.List;

import org.graalvm.compiler.core.common.cfg.BlockMap;
import org.graalvm.compiler.core.common.type.ArithmeticOpTable;
import org.graalvm.compiler.core.common.type.ArithmeticOpTable.BinaryOp;
import org.graalvm.compiler.core.common.type.ArithmeticOpTable.BinaryOp.And;
import org.graalvm.compiler.core.common.type.ArithmeticOpTable.BinaryOp.Or;
import org.graalvm.compiler.core.common.type.IntegerStamp;
import org.graalvm.compiler.core.common.type.ObjectStamp;
import org.graalvm.compiler.core.common.type.Stamp;
import org.graalvm.compiler.core.common.type.StampFactory;
import org.graalvm.compiler.debug.CounterKey;
import org.graalvm.compiler.debug.DebugCloseable;
import org.graalvm.compiler.debug.DebugContext;
import org.graalvm.compiler.graph.Node;
import org.graalvm.compiler.graph.NodeMap;
import org.graalvm.compiler.graph.NodeStack;
import org.graalvm.compiler.graph.spi.CanonicalizerTool;
import org.graalvm.compiler.nodeinfo.InputType;
import org.graalvm.compiler.nodes.AbstractBeginNode;
import org.graalvm.compiler.nodes.AbstractMergeNode;
import org.graalvm.compiler.nodes.BinaryOpLogicNode;
import org.graalvm.compiler.nodes.ConditionAnchorNode;
import org.graalvm.compiler.nodes.DeoptimizeNode;
import org.graalvm.compiler.nodes.DeoptimizingGuard;
import org.graalvm.compiler.nodes.EndNode;
import org.graalvm.compiler.nodes.FixedGuardNode;
import org.graalvm.compiler.nodes.FixedNode;
import org.graalvm.compiler.nodes.FixedWithNextNode;
import org.graalvm.compiler.nodes.GuardNode;
import org.graalvm.compiler.nodes.IfNode;
import org.graalvm.compiler.nodes.LogicNode;
import org.graalvm.compiler.nodes.LoopExitNode;
import org.graalvm.compiler.nodes.MergeNode;
import org.graalvm.compiler.nodes.NodeView;
import org.graalvm.compiler.nodes.ParameterNode;
import org.graalvm.compiler.nodes.PiNode;
import org.graalvm.compiler.nodes.ProxyNode;
import org.graalvm.compiler.nodes.ShortCircuitOrNode;
import org.graalvm.compiler.nodes.StructuredGraph;
import org.graalvm.compiler.nodes.StructuredGraph.ScheduleResult;
import org.graalvm.compiler.nodes.UnaryOpLogicNode;
import org.graalvm.compiler.nodes.ValueNode;
import org.graalvm.compiler.nodes.ValuePhiNode;
import org.graalvm.compiler.nodes.calc.AndNode;
import org.graalvm.compiler.nodes.calc.BinaryArithmeticNode;
import org.graalvm.compiler.nodes.calc.BinaryNode;
import org.graalvm.compiler.nodes.calc.IntegerEqualsNode;
import org.graalvm.compiler.nodes.calc.UnaryNode;
import org.graalvm.compiler.nodes.cfg.Block;
import org.graalvm.compiler.nodes.cfg.ControlFlowGraph;
import org.graalvm.compiler.nodes.extended.GuardingNode;
import org.graalvm.compiler.nodes.extended.IntegerSwitchNode;
import org.graalvm.compiler.nodes.extended.LoadHubNode;
import org.graalvm.compiler.nodes.extended.ValueAnchorNode;
import org.graalvm.compiler.nodes.java.TypeSwitchNode;
import org.graalvm.compiler.nodes.spi.NodeWithState;
import org.graalvm.compiler.nodes.spi.StampInverter;
import org.graalvm.compiler.nodes.util.GraphUtil;
import org.graalvm.compiler.phases.BasePhase;
import org.graalvm.compiler.phases.schedule.SchedulePhase;
import org.graalvm.compiler.phases.schedule.SchedulePhase.SchedulingStrategy;
import org.graalvm.compiler.phases.tiers.PhaseContext;
import org.graalvm.util.EconomicMap;
import org.graalvm.util.Equivalence;
import org.graalvm.util.MapCursor;
import org.graalvm.util.Pair;

import jdk.vm.ci.meta.JavaConstant;
import jdk.vm.ci.meta.TriState;

public class ConditionalEliminationPhase extends BasePhase<PhaseContext> {

    private static final CounterKey counterStampsRegistered = DebugContext.counter("StampsRegistered");
    private static final CounterKey counterStampsFound = DebugContext.counter("StampsFound");
    private static final CounterKey counterIfsKilled = DebugContext.counter("CE_KilledIfs");
    private static final CounterKey counterPhiStampsImproved = DebugContext.counter("CE_ImprovedPhis");
    private final boolean fullSchedule;
    private final boolean moveGuards;

    public ConditionalEliminationPhase(boolean fullSchedule) {
        this(fullSchedule, true);
    }

    public ConditionalEliminationPhase(boolean fullSchedule, boolean moveGuards) {
        this.fullSchedule = fullSchedule;
        this.moveGuards = moveGuards;
    }

    @Override
    @SuppressWarnings("try")
    protected void run(StructuredGraph graph, PhaseContext context) {
        try (DebugContext.Scope s = graph.getDebug().scope("DominatorConditionalElimination")) {
            BlockMap<List<Node>> blockToNodes = null;
            NodeMap<Block> nodeToBlock = null;
            ControlFlowGraph cfg = ControlFlowGraph.compute(graph, true, true, true, true);
            if (fullSchedule) {
                if (moveGuards) {
                    cfg.visitDominatorTree(new MoveGuardsUpwards(), graph.hasValueProxies());
                }
                SchedulePhase.run(graph, SchedulingStrategy.EARLIEST, cfg);
                ScheduleResult r = graph.getLastSchedule();
                blockToNodes = r.getBlockToNodesMap();
                nodeToBlock = r.getNodeToBlockMap();
            } else {
                nodeToBlock = cfg.getNodeToBlock();
                blockToNodes = getBlockToNodes(cfg);
            }
            ControlFlowGraph.RecursiveVisitor<?> visitor = createVisitor(graph, cfg, blockToNodes, nodeToBlock, context);
            cfg.visitDominatorTree(visitor, graph.hasValueProxies());
        }
    }

    protected BlockMap<List<Node>> getBlockToNodes(@SuppressWarnings("unused") ControlFlowGraph cfg) {
        return null;
    }

    protected ControlFlowGraph.RecursiveVisitor<?> createVisitor(StructuredGraph graph, @SuppressWarnings("unused") ControlFlowGraph cfg, BlockMap<List<Node>> blockToNodes,
                    @SuppressWarnings("unused") NodeMap<Block> nodeToBlock, PhaseContext context) {
        return new Instance(graph, blockToNodes, context);
    }

    public static class MoveGuardsUpwards implements ControlFlowGraph.RecursiveVisitor<Block> {

        Block anchorBlock;

        @Override
        @SuppressWarnings("try")
        public Block enter(Block b) {
            Block oldAnchorBlock = anchorBlock;
            if (b.getDominator() == null || b.getDominator().getPostdominator() != b) {
                // New anchor.
                anchorBlock = b;
            }

            AbstractBeginNode beginNode = b.getBeginNode();
            if (beginNode instanceof AbstractMergeNode && anchorBlock != b) {
                AbstractMergeNode mergeNode = (AbstractMergeNode) beginNode;
                for (GuardNode guard : mergeNode.guards().snapshot()) {
                    try (DebugCloseable closeable = guard.withNodeSourcePosition()) {
                        GuardNode newlyCreatedGuard = new GuardNode(guard.getCondition(), anchorBlock.getBeginNode(), guard.getReason(), guard.getAction(), guard.isNegated(), guard.getSpeculation());
                        GuardNode newGuard = mergeNode.graph().unique(newlyCreatedGuard);
                        guard.replaceAndDelete(newGuard);
                    }
                }
            }

            FixedNode endNode = b.getEndNode();
            if (endNode instanceof IfNode) {
                IfNode node = (IfNode) endNode;

                // Check if we can move guards upwards.
                AbstractBeginNode trueSuccessor = node.trueSuccessor();
                EconomicMap<LogicNode, GuardNode> trueGuards = EconomicMap.create(Equivalence.IDENTITY);
                for (GuardNode guard : trueSuccessor.guards()) {
                    LogicNode condition = guard.getCondition();
                    if (condition.hasMoreThanOneUsage()) {
                        trueGuards.put(condition, guard);
                    }
                }

                if (!trueGuards.isEmpty()) {
                    for (GuardNode guard : node.falseSuccessor().guards().snapshot()) {
                        GuardNode otherGuard = trueGuards.get(guard.getCondition());
                        if (otherGuard != null && guard.isNegated() == otherGuard.isNegated()) {
                            JavaConstant speculation = otherGuard.getSpeculation();
                            if (speculation == null) {
                                speculation = guard.getSpeculation();
                            } else if (guard.getSpeculation() != null && guard.getSpeculation() != speculation) {
                                // Cannot optimize due to different speculations.
                                continue;
                            }
                            try (DebugCloseable closeable = guard.withNodeSourcePosition()) {
                                GuardNode newlyCreatedGuard = new GuardNode(guard.getCondition(), anchorBlock.getBeginNode(), guard.getReason(), guard.getAction(), guard.isNegated(), speculation);
                                GuardNode newGuard = node.graph().unique(newlyCreatedGuard);
                                if (otherGuard.isAlive()) {
                                    otherGuard.replaceAndDelete(newGuard);
                                }
                                guard.replaceAndDelete(newGuard);
                            }
                        }
                    }
                }
            }
            return oldAnchorBlock;
        }

        @Override
        public void exit(Block b, Block value) {
            anchorBlock = value;
        }

    }

    private static final class PhiInfoElement {

        private EconomicMap<EndNode, InfoElement> infoElements;

        public void set(EndNode end, InfoElement infoElement) {
            if (infoElements == null) {
                infoElements = EconomicMap.create(Equivalence.IDENTITY);
            }
            infoElements.put(end, infoElement);
        }

        public InfoElement get(EndNode end) {
            if (infoElements == null) {
                return null;
            }
            return infoElements.get(end);
        }
    }

    public static class Instance implements ControlFlowGraph.RecursiveVisitor<Integer> {
        protected final NodeMap<InfoElement> map;
        protected final BlockMap<List<Node>> blockToNodes;
        protected final CanonicalizerTool tool;
        protected final NodeStack undoOperations;
        protected final StructuredGraph graph;
        protected final DebugContext debug;
        protected final EconomicMap<MergeNode, EconomicMap<ValuePhiNode, PhiInfoElement>> mergeMaps;

        /**
         * Tests which may be eliminated because post dominating tests to prove a broader condition.
         */
        private Deque<DeoptimizingGuard> pendingTests;

        public Instance(StructuredGraph graph, BlockMap<List<Node>> blockToNodes, PhaseContext context) {
            this.graph = graph;
            this.debug = graph.getDebug();
            this.blockToNodes = blockToNodes;
            this.undoOperations = new NodeStack();
            this.map = graph.createNodeMap();
            pendingTests = new ArrayDeque<>();
            tool = GraphUtil.getDefaultSimplifier(context.getMetaAccess(), context.getConstantReflection(), context.getConstantFieldProvider(), false, graph.getAssumptions(), graph.getOptions(),
                            context.getLowerer());
            mergeMaps = EconomicMap.create();
        }

        protected void processConditionAnchor(ConditionAnchorNode node) {
            tryProveCondition(node.condition(), (guard, result, guardedValueStamp, newInput) -> {
                if (result != node.isNegated()) {
                    node.replaceAtUsages(guard.asNode());
                    GraphUtil.unlinkFixedNode(node);
                    GraphUtil.killWithUnusedFloatingInputs(node);
                } else {
                    ValueAnchorNode valueAnchor = node.graph().add(new ValueAnchorNode(null));
                    node.replaceAtUsages(valueAnchor);
                    node.graph().replaceFixedWithFixed(node, valueAnchor);
                }
                return true;
            });
        }

        protected void processGuard(GuardNode node) {
            if (!tryProveGuardCondition(node, node.getCondition(), (guard, result, guardedValueStamp, newInput) -> {
                if (result != node.isNegated()) {
                    node.replaceAndDelete(guard.asNode());
                } else {
                    DeoptimizeNode deopt = node.graph().add(new DeoptimizeNode(node.getAction(), node.getReason(), node.getSpeculation()));
                    AbstractBeginNode beginNode = (AbstractBeginNode) node.getAnchor();
                    FixedNode next = beginNode.next();
                    beginNode.setNext(deopt);
                    GraphUtil.killCFG(next);
                }
                return true;
            })) {
                registerNewCondition(node.getCondition(), node.isNegated(), node);
            }
        }

        protected void processFixedGuard(FixedGuardNode node) {
            if (!tryProveGuardCondition(node, node.condition(), (guard, result, guardedValueStamp, newInput) -> {
                if (result != node.isNegated()) {
                    node.replaceAtUsages(guard.asNode());
                    GraphUtil.unlinkFixedNode(node);
                    GraphUtil.killWithUnusedFloatingInputs(node);
                } else {
                    DeoptimizeNode deopt = node.graph().add(new DeoptimizeNode(node.getAction(), node.getReason(), node.getSpeculation()));
                    deopt.setStateBefore(node.stateBefore());
                    node.replaceAtPredecessor(deopt);
                    GraphUtil.killCFG(node);
                }
                debug.log("Kill fixed guard guard");
                return true;
            })) {
                registerNewCondition(node.condition(), node.isNegated(), node);
            }
        }

        protected void processIf(IfNode node) {
            tryProveCondition(node.condition(), (guard, result, guardedValueStamp, newInput) -> {
                AbstractBeginNode survivingSuccessor = node.getSuccessor(result);
                survivingSuccessor.replaceAtUsages(InputType.Guard, guard.asNode());
                survivingSuccessor.replaceAtPredecessor(null);
                node.replaceAtPredecessor(survivingSuccessor);
                GraphUtil.killCFG(node);
                counterIfsKilled.increment(debug);
                return true;
            });
        }

        @Override
        public Integer enter(Block block) {
            int mark = undoOperations.size();
            debug.log("[Pre Processing block %s]", block);
            // For now conservatively collect guards only within the same block.
            pendingTests.clear();
            processNodes(block);
            return mark;
        }

        protected void processNodes(Block block) {
            if (blockToNodes != null) {
                for (Node n : blockToNodes.get(block)) {
                    if (n.isAlive()) {
                        processNode(n);
                    }
                }
            } else {
                processBlock(block);
            }
        }

        private void processBlock(Block block) {
            FixedNode n = block.getBeginNode();
            FixedNode endNode = block.getEndNode();
            debug.log("[Processing block %s]", block);
            while (n != endNode) {
                if (n.isDeleted() || endNode.isDeleted()) {
                    // This branch was deleted!
                    return;
                }
                FixedNode next = ((FixedWithNextNode) n).next();
                processNode(n);
                n = next;
            }
            if (endNode.isAlive()) {
                processNode(endNode);
            }
        }

        @SuppressWarnings("try")
        protected void processNode(Node node) {
            try (DebugCloseable closeable = node.withNodeSourcePosition()) {
                if (node instanceof NodeWithState && !(node instanceof GuardingNode)) {
                    pendingTests.clear();
                }

                if (node instanceof MergeNode) {
                    introducePisForPhis((MergeNode) node);
                }

                if (node instanceof AbstractBeginNode) {
                    if (node instanceof LoopExitNode && graph.hasValueProxies()) {
                        // Condition must not be used down this path.
                        return;
                    }
                    processAbstractBegin((AbstractBeginNode) node);
                } else if (node instanceof FixedGuardNode) {
                    processFixedGuard((FixedGuardNode) node);
                } else if (node instanceof GuardNode) {
                    processGuard((GuardNode) node);
                } else if (node instanceof ConditionAnchorNode) {
                    processConditionAnchor((ConditionAnchorNode) node);
                } else if (node instanceof IfNode) {
                    processIf((IfNode) node);
                } else if (node instanceof EndNode) {
                    processEnd((EndNode) node);
                }
            }
        }

        protected void introducePisForPhis(MergeNode merge) {
            EconomicMap<ValuePhiNode, PhiInfoElement> mergeMap = this.mergeMaps.get(merge);
            if (mergeMap != null) {
                MapCursor<ValuePhiNode, PhiInfoElement> entries = mergeMap.getEntries();
                while (entries.advance()) {
                    ValuePhiNode phi = entries.getKey();
                    assert phi.isAlive() || phi.isDeleted();
                    /*
                     * Phi might have been killed already via a conditional elimination in another
                     * branch.
                     */
                    if (phi.isDeleted()) {
                        continue;
                    }
                    PhiInfoElement phiInfoElements = entries.getValue();
                    Stamp bestPossibleStamp = null;
                    for (int i = 0; i < phi.valueCount(); ++i) {
                        ValueNode valueAt = phi.valueAt(i);
                        Stamp curBestStamp = valueAt.stamp(NodeView.DEFAULT);
                        InfoElement infoElement = phiInfoElements.get(merge.forwardEndAt(i));
                        if (infoElement != null) {
                            curBestStamp = curBestStamp.join(infoElement.getStamp());
                        }

                        if (bestPossibleStamp == null) {
                            bestPossibleStamp = curBestStamp;
                        } else {
                            bestPossibleStamp = bestPossibleStamp.meet(curBestStamp);
                        }
                    }

                    Stamp oldStamp = phi.stamp(NodeView.DEFAULT);
                    if (oldStamp.tryImproveWith(bestPossibleStamp) != null) {

                        // Need to be careful to not run into stamp update cycles with the iterative
                        // canonicalization.
                        boolean allow = false;
                        if (bestPossibleStamp instanceof ObjectStamp) {
                            // Always allow object stamps.
                            allow = true;
                        } else if (bestPossibleStamp instanceof IntegerStamp) {
                            IntegerStamp integerStamp = (IntegerStamp) bestPossibleStamp;
                            IntegerStamp oldIntegerStamp = (IntegerStamp) oldStamp;
                            if (integerStamp.isPositive() != oldIntegerStamp.isPositive()) {
                                allow = true;
                            } else if (integerStamp.isNegative() != oldIntegerStamp.isNegative()) {
                                allow = true;
                            } else if (integerStamp.isStrictlyPositive() != oldIntegerStamp.isStrictlyPositive()) {
                                allow = true;
                            } else if (integerStamp.isStrictlyNegative() != oldIntegerStamp.isStrictlyNegative()) {
                                allow = true;
                            } else if (integerStamp.asConstant() != null) {
                                allow = true;
                            } else if (oldStamp.isUnrestricted()) {
                                allow = true;
                            }
                        } else {
                            allow = (bestPossibleStamp.asConstant() != null);
                        }

                        if (allow) {
                            ValuePhiNode newPhi = graph.addWithoutUnique(new ValuePhiNode(bestPossibleStamp, merge));
                            for (int i = 0; i < phi.valueCount(); ++i) {
                                ValueNode valueAt = phi.valueAt(i);
                                if (bestPossibleStamp.meet(valueAt.stamp(NodeView.DEFAULT)).equals(bestPossibleStamp)) {
                                    // Pi not required here.
                                } else {
                                    InfoElement infoElement = phiInfoElements.get(merge.forwardEndAt(i));
                                    assert infoElement != null;
                                    Stamp curBestStamp = infoElement.getStamp();
                                    ValueNode input = infoElement.getProxifiedInput();
                                    if (input == null) {
                                        input = valueAt;
                                    }
                                    ValueNode valueNode = graph.maybeAddOrUnique(PiNode.create(input, curBestStamp, (ValueNode) infoElement.guard));
                                    valueAt = valueNode;
                                }
                                newPhi.addInput(valueAt);
                            }
                            counterPhiStampsImproved.increment(debug);
                            phi.replaceAtUsagesAndDelete(newPhi);
                        }
                    }
                }
            }
        }

        protected void processEnd(EndNode end) {
            AbstractMergeNode abstractMerge = end.merge();
            if (abstractMerge instanceof MergeNode) {
                MergeNode merge = (MergeNode) abstractMerge;

                EconomicMap<ValuePhiNode, PhiInfoElement> mergeMap = this.mergeMaps.get(merge);
                for (ValuePhiNode phi : merge.valuePhis()) {
                    ValueNode valueAt = phi.valueAt(end);
                    InfoElement infoElement = this.getInfoElements(valueAt);
                    while (infoElement != null) {
                        Stamp newStamp = infoElement.getStamp();
                        if (phi.stamp(NodeView.DEFAULT).tryImproveWith(newStamp) != null) {
                            if (mergeMap == null) {
                                mergeMap = EconomicMap.create();
                                mergeMaps.put(merge, mergeMap);
                            }

                            PhiInfoElement phiInfoElement = mergeMap.get(phi);
                            if (phiInfoElement == null) {
                                phiInfoElement = new PhiInfoElement();
                                mergeMap.put(phi, phiInfoElement);
                            }

                            phiInfoElement.set(end, infoElement);
                            break;
                        }
                        infoElement = nextElement(infoElement);
                    }
                }
            }
        }

        protected void registerNewCondition(LogicNode condition, boolean negated, GuardingNode guard) {
            if (condition instanceof UnaryOpLogicNode) {
                UnaryOpLogicNode unaryLogicNode = (UnaryOpLogicNode) condition;
                ValueNode value = unaryLogicNode.getValue();
                if (maybeMultipleUsages(value)) {
                    Stamp newStamp = unaryLogicNode.getSucceedingStampForValue(negated);
                    registerNewStamp(value, newStamp, guard);
                }
            } else if (condition instanceof BinaryOpLogicNode) {
                BinaryOpLogicNode binaryOpLogicNode = (BinaryOpLogicNode) condition;
                ValueNode x = binaryOpLogicNode.getX();
                ValueNode y = binaryOpLogicNode.getY();
                if (!x.isConstant() && maybeMultipleUsages(x)) {
                    Stamp newStampX = binaryOpLogicNode.getSucceedingStampForX(negated, getSafeStamp(x), getOtherSafeStamp(y));
                    registerNewStamp(x, newStampX, guard);
                }

                if (!y.isConstant() && maybeMultipleUsages(y)) {
                    Stamp newStampY = binaryOpLogicNode.getSucceedingStampForY(negated, getOtherSafeStamp(x), getSafeStamp(y));
                    registerNewStamp(y, newStampY, guard);
                }

                if (condition instanceof IntegerEqualsNode && guard instanceof DeoptimizingGuard && !negated) {
                    if (y.isConstant() && x instanceof AndNode) {
                        AndNode and = (AndNode) x;
                        ValueNode andX = and.getX();
                        if (and.getY() == y && maybeMultipleUsages(andX)) {
                            /*
                             * This 'and' proves something about some of the bits in and.getX().
                             * It's equivalent to or'ing in the mask value since those values are
                             * known to be set.
                             */
                            BinaryOp<Or> op = ArithmeticOpTable.forStamp(x.stamp(NodeView.DEFAULT)).getOr();
                            IntegerStamp newStampX = (IntegerStamp) op.foldStamp(getSafeStamp(andX), getOtherSafeStamp(y));
                            registerNewStamp(andX, newStampX, guard);
                        }
                    }
                }
            }
            if (guard instanceof DeoptimizingGuard) {
                assert ((DeoptimizingGuard) guard).getCondition() == condition;
                pendingTests.push((DeoptimizingGuard) guard);
            }
            registerCondition(condition, negated, guard);
        }

        Pair<InfoElement, Stamp> recursiveFoldStamp(Node node) {
            if (node instanceof UnaryNode) {
                UnaryNode unary = (UnaryNode) node;
                ValueNode value = unary.getValue();
                InfoElement infoElement = getInfoElements(value);
                while (infoElement != null) {
                    Stamp result = unary.foldStamp(infoElement.getStamp());
                    if (result != null) {
                        return Pair.create(infoElement, result);
                    }
                    infoElement = nextElement(infoElement);
                }
            } else if (node instanceof BinaryNode) {
                BinaryNode binary = (BinaryNode) node;
                ValueNode y = binary.getY();
                ValueNode x = binary.getX();
                if (y.isConstant()) {
                    InfoElement infoElement = getInfoElements(x);
                    while (infoElement != null) {
                        Stamp result = binary.foldStamp(infoElement.stamp, y.stamp(NodeView.DEFAULT));
                        if (result != null) {
                            return Pair.create(infoElement, result);
                        }
                        infoElement = nextElement(infoElement);
                    }
                }
            }
            return null;
        }

        /**
         * Get the stamp that may be used for the value for which we are registering the condition.
         * We may directly use the stamp here without restriction, because any later lookup of the
         * registered info elements is in the same chain of pi nodes.
         */
        private static Stamp getSafeStamp(ValueNode x) {
            return x.stamp(NodeView.DEFAULT);
        }

        /**
         * We can only use the stamp of a second value involved in the condition if we are sure that
         * we are not implicitly creating a dependency on a pi node that is responsible for that
         * stamp. For now, we are conservatively only using the stamps of constants. Under certain
         * circumstances, we may also be able to use the stamp of the value after skipping pi nodes
         * (e.g., the stamp of a parameter after inlining, or the stamp of a fixed node that can
         * never be replaced with a pi node via canonicalization).
         */
        private static Stamp getOtherSafeStamp(ValueNode x) {
            if (x.isConstant()) {
                return x.stamp(NodeView.DEFAULT);
            }
            return x.stamp(NodeView.DEFAULT).unrestricted();
        }

        /**
         * Recursively try to fold stamps within this expression using information from
         * {@link #getInfoElements(ValueNode)}. It's only safe to use constants and one
         * {@link InfoElement} otherwise more than one guard would be required.
         *
         * @param node
         * @return the pair of the @{link InfoElement} used and the stamp produced for the whole
         *         expression
         */
        Pair<InfoElement, Stamp> recursiveFoldStampFromInfo(Node node) {
            return recursiveFoldStamp(node);
        }

        protected boolean foldPendingTest(DeoptimizingGuard thisGuard, ValueNode original, Stamp newStamp, GuardRewirer rewireGuardFunction) {
            for (DeoptimizingGuard pendingGuard : pendingTests) {
                LogicNode pendingCondition = pendingGuard.getCondition();
                TriState result = TriState.UNKNOWN;
                if (pendingCondition instanceof UnaryOpLogicNode) {
                    UnaryOpLogicNode unaryLogicNode = (UnaryOpLogicNode) pendingCondition;
                    if (unaryLogicNode.getValue() == original) {
                        result = unaryLogicNode.tryFold(newStamp);
                    }
                } else if (pendingCondition instanceof BinaryOpLogicNode) {
                    BinaryOpLogicNode binaryOpLogicNode = (BinaryOpLogicNode) pendingCondition;
                    ValueNode x = binaryOpLogicNode.getX();
                    ValueNode y = binaryOpLogicNode.getY();
                    if (x == original) {
                        result = binaryOpLogicNode.tryFold(newStamp, getOtherSafeStamp(y));
                    } else if (y == original) {
                        result = binaryOpLogicNode.tryFold(getOtherSafeStamp(x), newStamp);
                    } else if (binaryOpLogicNode instanceof IntegerEqualsNode && y.isConstant() && x instanceof AndNode) {
                        AndNode and = (AndNode) x;
                        if (and.getY() == y && and.getX() == original) {
                            BinaryOp<And> andOp = ArithmeticOpTable.forStamp(newStamp).getAnd();
                            result = binaryOpLogicNode.tryFold(andOp.foldStamp(newStamp, getOtherSafeStamp(y)), getOtherSafeStamp(y));
                        }
                    }
                }
                if (result.isKnown()) {
                    /*
                     * The test case be folded using the information available but the test can only
                     * be moved up if we're sure there's no schedule dependence. For now limit it to
                     * the original node and constants.
                     */
                    InputFilter v = new InputFilter(original);
                    thisGuard.getCondition().applyInputs(v);
                    if (v.ok && foldGuard(thisGuard, pendingGuard, newStamp, rewireGuardFunction)) {
                        return true;
                    }
                }
            }
            return false;
        }

        protected boolean foldGuard(DeoptimizingGuard thisGuard, DeoptimizingGuard otherGuard, Stamp guardedValueStamp, GuardRewirer rewireGuardFunction) {
            if (otherGuard.getAction() == thisGuard.getAction() && otherGuard.getSpeculation() == thisGuard.getSpeculation()) {
                LogicNode condition = (LogicNode) thisGuard.getCondition().copyWithInputs();
                GuardRewirer rewirer = (guard, result, innerGuardedValueStamp, newInput) -> {
                    if (rewireGuardFunction.rewire(guard, result, innerGuardedValueStamp, newInput)) {
                        otherGuard.setCondition(condition, thisGuard.isNegated());
                        return true;
                    }
                    condition.safeDelete();
                    return false;
                };
                // Move the later test up
                return rewireGuards(otherGuard, !thisGuard.isNegated(), null, guardedValueStamp, rewirer);
            }
            return false;
        }

        protected void registerCondition(LogicNode condition, boolean negated, GuardingNode guard) {
            if (condition.getUsageCount() > 1) {
                registerNewStamp(condition, negated ? StampFactory.contradiction() : StampFactory.tautology(), guard);
            }
        }

        protected InfoElement getInfoElements(ValueNode proxiedValue) {
            ValueNode value = GraphUtil.skipPi(proxiedValue);
            if (value == null) {
                return null;
            }
            return map.getAndGrow(value);
        }

        protected boolean rewireGuards(GuardingNode guard, boolean result, ValueNode proxifiedInput, Stamp guardedValueStamp, GuardRewirer rewireGuardFunction) {
            counterStampsFound.increment(debug);
            return rewireGuardFunction.rewire(guard, result, guardedValueStamp, proxifiedInput);
        }

        protected boolean tryProveCondition(LogicNode node, GuardRewirer rewireGuardFunction) {
            return tryProveGuardCondition(null, node, rewireGuardFunction);
        }

        private InfoElement nextElement(InfoElement current) {
            InfoElement parent = current.getParent();
            if (parent != null) {
                return parent;
            } else {
                ValueNode proxifiedInput = current.getProxifiedInput();
                if (proxifiedInput instanceof PiNode) {
                    PiNode piNode = (PiNode) proxifiedInput;
                    return getInfoElements(piNode.getOriginalNode());
                }
            }
            return null;
        }

        protected boolean tryProveGuardCondition(DeoptimizingGuard thisGuard, LogicNode node, GuardRewirer rewireGuardFunction) {
            InfoElement infoElement = getInfoElements(node);
            while (infoElement != null) {
                Stamp stamp = infoElement.getStamp();
                JavaConstant constant = (JavaConstant) stamp.asConstant();
                if (constant != null) {
                    // No proxified input and stamp required.
                    return rewireGuards(infoElement.getGuard(), constant.asBoolean(), null, null, rewireGuardFunction);
                }
                infoElement = nextElement(infoElement);
            }

            if (node instanceof UnaryOpLogicNode) {
                UnaryOpLogicNode unaryLogicNode = (UnaryOpLogicNode) node;
                ValueNode value = unaryLogicNode.getValue();
                infoElement = getInfoElements(value);
                while (infoElement != null) {
                    Stamp stamp = infoElement.getStamp();
                    TriState result = unaryLogicNode.tryFold(stamp);
                    if (result.isKnown()) {
                        return rewireGuards(infoElement.getGuard(), result.toBoolean(), infoElement.getProxifiedInput(), infoElement.getStamp(), rewireGuardFunction);
                    }
                    infoElement = nextElement(infoElement);
                }
                Pair<InfoElement, Stamp> foldResult = recursiveFoldStampFromInfo(value);
                if (foldResult != null) {
                    TriState result = unaryLogicNode.tryFold(foldResult.getRight());
                    if (result.isKnown()) {
                        return rewireGuards(foldResult.getLeft().getGuard(), result.toBoolean(), foldResult.getLeft().getProxifiedInput(), foldResult.getRight(), rewireGuardFunction);
                    }
                }
                if (thisGuard != null) {
                    Stamp newStamp = unaryLogicNode.getSucceedingStampForValue(thisGuard.isNegated());
                    if (newStamp != null && foldPendingTest(thisGuard, value, newStamp, rewireGuardFunction)) {
                        return true;
                    }

                }
            } else if (node instanceof BinaryOpLogicNode) {
                BinaryOpLogicNode binaryOpLogicNode = (BinaryOpLogicNode) node;
                infoElement = getInfoElements(binaryOpLogicNode);
                while (infoElement != null) {
                    if (infoElement.getStamp().equals(StampFactory.contradiction())) {
                        return rewireGuards(infoElement.getGuard(), false, infoElement.getProxifiedInput(), null, rewireGuardFunction);
                    } else if (infoElement.getStamp().equals(StampFactory.tautology())) {
                        return rewireGuards(infoElement.getGuard(), true, infoElement.getProxifiedInput(), null, rewireGuardFunction);
                    }
                    infoElement = nextElement(infoElement);
                }

                ValueNode x = binaryOpLogicNode.getX();
                ValueNode y = binaryOpLogicNode.getY();
                infoElement = getInfoElements(x);
                while (infoElement != null) {
                    TriState result = binaryOpLogicNode.tryFold(infoElement.getStamp(), y.stamp(NodeView.DEFAULT));
                    if (result.isKnown()) {
                        return rewireGuards(infoElement.getGuard(), result.toBoolean(), infoElement.getProxifiedInput(), infoElement.getStamp(), rewireGuardFunction);
                    }
                    infoElement = nextElement(infoElement);
                }

                if (y.isConstant()) {
                    Pair<InfoElement, Stamp> foldResult = recursiveFoldStampFromInfo(x);
                    if (foldResult != null) {
                        TriState result = binaryOpLogicNode.tryFold(foldResult.getRight(), y.stamp(NodeView.DEFAULT));
                        if (result.isKnown()) {
                            return rewireGuards(foldResult.getLeft().getGuard(), result.toBoolean(), foldResult.getLeft().getProxifiedInput(), foldResult.getRight(), rewireGuardFunction);
                        }
                    }
                } else {
                    infoElement = getInfoElements(y);
                    while (infoElement != null) {
                        TriState result = binaryOpLogicNode.tryFold(x.stamp(NodeView.DEFAULT), infoElement.getStamp());
                        if (result.isKnown()) {
                            return rewireGuards(infoElement.getGuard(), result.toBoolean(), infoElement.getProxifiedInput(), infoElement.getStamp(), rewireGuardFunction);
                        }
                        infoElement = nextElement(infoElement);
                    }
                }

                /*
                 * For complex expressions involving constants, see if it's possible to fold the
                 * tests by using stamps one level up in the expression. For instance, (x + n < y)
                 * might fold if something is known about x and all other values are constants. The
                 * reason for the constant restriction is that if more than 1 real value is involved
                 * the code might need to adopt multiple guards to have proper dependences.
                 */
                if (x instanceof BinaryArithmeticNode<?> && y.isConstant()) {
                    BinaryArithmeticNode<?> binary = (BinaryArithmeticNode<?>) x;
                    if (binary.getY().isConstant()) {
                        infoElement = getInfoElements(binary.getX());
                        while (infoElement != null) {
                            Stamp newStampX = binary.foldStamp(infoElement.getStamp(), binary.getY().stamp(NodeView.DEFAULT));
                            TriState result = binaryOpLogicNode.tryFold(newStampX, y.stamp(NodeView.DEFAULT));
                            if (result.isKnown()) {
                                return rewireGuards(infoElement.getGuard(), result.toBoolean(), infoElement.getProxifiedInput(), newStampX, rewireGuardFunction);
                            }
                            infoElement = nextElement(infoElement);
                        }
                    }
                }

                if (thisGuard != null && binaryOpLogicNode instanceof IntegerEqualsNode && !thisGuard.isNegated()) {
                    if (y.isConstant() && x instanceof AndNode) {
                        AndNode and = (AndNode) x;
                        if (and.getY() == y) {
                            /*
                             * This 'and' proves something about some of the bits in and.getX().
                             * It's equivalent to or'ing in the mask value since those values are
                             * known to be set.
                             */
                            BinaryOp<Or> op = ArithmeticOpTable.forStamp(x.stamp(NodeView.DEFAULT)).getOr();
                            IntegerStamp newStampX = (IntegerStamp) op.foldStamp(getSafeStamp(and.getX()), getOtherSafeStamp(y));
                            if (foldPendingTest(thisGuard, and.getX(), newStampX, rewireGuardFunction)) {
                                return true;
                            }
                        }
                    }
                }

                if (thisGuard != null) {
                    if (!x.isConstant()) {
                        Stamp newStampX = binaryOpLogicNode.getSucceedingStampForX(thisGuard.isNegated(), getSafeStamp(x), getOtherSafeStamp(y));
                        if (newStampX != null && foldPendingTest(thisGuard, x, newStampX, rewireGuardFunction)) {
                            return true;
                        }
                    }
                    if (!y.isConstant()) {
                        Stamp newStampY = binaryOpLogicNode.getSucceedingStampForY(thisGuard.isNegated(), getOtherSafeStamp(x), getSafeStamp(y));
                        if (newStampY != null && foldPendingTest(thisGuard, y, newStampY, rewireGuardFunction)) {
                            return true;
                        }
                    }
                }
            } else if (node instanceof ShortCircuitOrNode) {
                final ShortCircuitOrNode shortCircuitOrNode = (ShortCircuitOrNode) node;
                return tryProveCondition(shortCircuitOrNode.getX(), (guard, result, guardedValueStamp, newInput) -> {
                    if (result == !shortCircuitOrNode.isXNegated()) {
                        return rewireGuards(guard, true, newInput, guardedValueStamp, rewireGuardFunction);
                    } else {
                        return tryProveCondition(shortCircuitOrNode.getY(), (innerGuard, innerResult, innerGuardedValueStamp, innerNewInput) -> {
                            ValueNode proxifiedInput = newInput;
                            if (proxifiedInput == null) {
                                proxifiedInput = innerNewInput;
                            } else if (innerNewInput != null) {
                                if (innerNewInput != newInput) {
                                    // Cannot canonicalize due to different proxied inputs.
                                    return false;
                                }
                            }
                            // Can only canonicalize if the guards are equal.
                            if (innerGuard == guard) {
                                return rewireGuards(guard, innerResult ^ shortCircuitOrNode.isYNegated(), proxifiedInput, guardedValueStamp, rewireGuardFunction);
                            }
                            return false;
                        });
                    }
                });
            }

            return false;
        }

        protected void registerNewStamp(ValueNode maybeProxiedValue, Stamp newStamp, GuardingNode guard) {
            assert maybeProxiedValue != null;
            assert guard != null;
            if (newStamp != null) {
                ValueNode value = maybeProxiedValue;
                Stamp stamp = newStamp;
                ValueNode proxiedValue = null;
                if (value instanceof PiNode) {
                    proxiedValue = value;
                }
                do {
                    counterStampsRegistered.increment(debug);
                    debug.log("\t Saving stamp for node %s stamp %s guarded by %s", value, stamp, guard);
                    assert value instanceof LogicNode || stamp.isCompatible(value.stamp(NodeView.DEFAULT)) : stamp + " vs. " + value.stamp(NodeView.DEFAULT) + " (" + value + ")";
                    map.setAndGrow(value, new InfoElement(stamp, guard, proxiedValue, map.getAndGrow(value)));
                    undoOperations.push(value);
                    if (value instanceof StampInverter) {
                        StampInverter stampInverter = (StampInverter) value;
                        value = stampInverter.getValue();
                        stamp = stampInverter.invertStamp(stamp);
                    } else {
                        value = null;
                        stamp = null;
                    }
                } while (value != null && stamp != null);
            }
        }

        protected void processAbstractBegin(AbstractBeginNode beginNode) {
            Node predecessor = beginNode.predecessor();
            if (predecessor instanceof IfNode) {
                IfNode ifNode = (IfNode) predecessor;
                boolean negated = (ifNode.falseSuccessor() == beginNode);
                LogicNode condition = ifNode.condition();
                registerNewCondition(condition, negated, beginNode);
            } else if (predecessor instanceof TypeSwitchNode) {
                TypeSwitchNode typeSwitch = (TypeSwitchNode) predecessor;
                processTypeSwitch(beginNode, typeSwitch);
            } else if (predecessor instanceof IntegerSwitchNode) {
                IntegerSwitchNode integerSwitchNode = (IntegerSwitchNode) predecessor;
                processIntegerSwitch(beginNode, integerSwitchNode);
            }
        }

        private static boolean maybeMultipleUsages(ValueNode value) {
            if (value.hasMoreThanOneUsage()) {
                return true;
            } else {
                return value instanceof ProxyNode;
            }
        }

        protected void processIntegerSwitch(AbstractBeginNode beginNode, IntegerSwitchNode integerSwitchNode) {
            ValueNode value = integerSwitchNode.value();
            if (maybeMultipleUsages(value)) {
                Stamp stamp = integerSwitchNode.getValueStampForSuccessor(beginNode);
                if (stamp != null) {
                    registerNewStamp(value, stamp, beginNode);
                }
            }
        }

        protected void processTypeSwitch(AbstractBeginNode beginNode, TypeSwitchNode typeSwitch) {
            ValueNode hub = typeSwitch.value();
            if (hub instanceof LoadHubNode) {
                LoadHubNode loadHub = (LoadHubNode) hub;
                ValueNode value = loadHub.getValue();
                if (maybeMultipleUsages(value)) {
                    Stamp stamp = typeSwitch.getValueStampForSuccessor(beginNode);
                    if (stamp != null) {
                        registerNewStamp(value, stamp, beginNode);
                    }
                }
            }
        }

        @Override
        public void exit(Block b, Integer state) {
            int mark = state;
            while (undoOperations.size() > mark) {
                Node node = undoOperations.pop();
                if (node.isAlive()) {
                    map.set(node, map.get(node).getParent());
                }
            }
        }
    }

    @FunctionalInterface
    protected interface InfoElementProvider {
        Iterable<InfoElement> getInfoElements(ValueNode value);
    }

    /**
     * Checks for safe nodes when moving pending tests up.
     */
    static class InputFilter extends Node.EdgeVisitor {
        boolean ok;
        private ValueNode value;

        InputFilter(ValueNode value) {
            this.value = value;
            this.ok = true;
        }

        @Override
        public Node apply(Node node, Node curNode) {
            if (!ok) {
                // Abort the recursion
                return curNode;
            }
            if (!(curNode instanceof ValueNode)) {
                ok = false;
                return curNode;
            }
            ValueNode curValue = (ValueNode) curNode;
            if (curValue.isConstant() || curValue == value || curValue instanceof ParameterNode) {
                return curNode;
            }
            if (curValue instanceof BinaryNode || curValue instanceof UnaryNode) {
                curValue.applyInputs(this);
            } else {
                ok = false;
            }
            return curNode;
        }
    }

    @FunctionalInterface
    protected interface GuardRewirer {
        /**
         * Called if the condition could be proven to have a constant value ({@code result}) under
         * {@code guard}.
         *
         * @param guard the guard whose result is proven
         * @param result the known result of the guard
         * @param newInput new input to pi nodes depending on the new guard
         * @return whether the transformation could be applied
         */
        boolean rewire(GuardingNode guard, boolean result, Stamp guardedValueStamp, ValueNode newInput);
    }

    protected static final class InfoElement {
        private final Stamp stamp;
        private final GuardingNode guard;
        private final ValueNode proxifiedInput;
        private final InfoElement parent;

        public InfoElement(Stamp stamp, GuardingNode guard, ValueNode proxifiedInput, InfoElement parent) {
            this.stamp = stamp;
            this.guard = guard;
            this.proxifiedInput = proxifiedInput;
            this.parent = parent;
        }

        public InfoElement getParent() {
            return parent;
        }

        public Stamp getStamp() {
            return stamp;
        }

        public GuardingNode getGuard() {
            return guard;
        }

        public ValueNode getProxifiedInput() {
            return proxifiedInput;
        }

        @Override
        public String toString() {
            return stamp + " -> " + guard;
        }
    }

    @Override
    public float codeSizeIncrease() {
        return 1.5f;
    }
}
