// SPDX-License-Identifier: BSD-3-Clause
/* Copyright 2020-2022, Intel Corporation */
/* Copyright 2021-2022, Fujitsu */

/*
 * peer-create_qp.c -- a peer unit test
 *
 * API covered:
 * - rpma_peer_setup_qp()
 */

#include <infiniband/verbs.h>

#include "cmocka_headers.h"
#include "conn_req.h"
#include "mocks-ibverbs.h"
#include "mocks-rpma-conn_cfg.h"
#include "mocks-rpma-cq.h"
#include "mocks-rpma-srq.h"
#include "peer.h"
#include "peer-common.h"

#define MOCK_GET_IBV_RCQ(rcq) rcq == MOCK_RPMA_RCQ ? MOCK_IBV_RCQ : MOCK_IBV_SRQ_RCQ

static struct conn_cfg_get_mock_args Get_args = {
	.cfg = MOCK_CONN_CFG_CUSTOM,
	.sq_size = MOCK_SQ_SIZE_CUSTOM,
	.rq_size = MOCK_RQ_SIZE_CUSTOM,
};

static struct rpma_cq *rcqs[] = {
	NULL,
	MOCK_RPMA_RCQ,
	MOCK_RPMA_SRQ_RCQ,
};

static int num_rcqs = sizeof(rcqs) / sizeof(rcqs[0]);

/*
 * configure_create_qp -- configure common mock for rdma_create_qp()
 */
static void
configure_create_qp(struct rpma_cq *rcq)
{
	will_return(rpma_conn_cfg_get_sq_size, &Get_args);
	will_return(rpma_conn_cfg_get_rq_size, &Get_args);
	will_return(rpma_conn_cfg_get_srq, &Get_args);
	if (Get_args.srq) {
		expect_value(rpma_srq_get_ibv_srq, srq, MOCK_RPMA_SRQ);
		will_return(rpma_srq_get_ibv_srq, MOCK_IBV_SRQ);
	}
	expect_value(rpma_cq_get_ibv_cq, cq, MOCK_RPMA_CQ);
	will_return(rpma_cq_get_ibv_cq, MOCK_IBV_CQ);
	if (rcq) {
		expect_value(rpma_cq_get_ibv_cq, cq, rcq);
		will_return(rpma_cq_get_ibv_cq, MOCK_GET_IBV_RCQ(rcq));
	}
	expect_value(rdma_create_qp, id, MOCK_CM_ID);
	expect_value(rdma_create_qp, pd, MOCK_IBV_PD);
	expect_value(rdma_create_qp, qp_init_attr->qp_context, NULL);
	expect_value(rdma_create_qp, qp_init_attr->send_cq,
		MOCK_IBV_CQ);
	expect_value(rdma_create_qp, qp_init_attr->recv_cq,
		rcq ? MOCK_GET_IBV_RCQ(rcq) : MOCK_IBV_CQ);
	expect_value(rdma_create_qp, qp_init_attr->srq, Get_args.srq ?
		MOCK_IBV_SRQ : NULL);
	expect_value(rdma_create_qp, qp_init_attr->cap.max_send_wr,
		MOCK_SQ_SIZE_CUSTOM);
	expect_value(rdma_create_qp, qp_init_attr->cap.max_recv_wr,
		MOCK_RQ_SIZE_CUSTOM);
	expect_value(rdma_create_qp, qp_init_attr->cap.max_send_sge,
		RPMA_MAX_SGE);
	expect_value(rdma_create_qp, qp_init_attr->cap.max_recv_sge,
		RPMA_MAX_SGE);
	expect_value(rdma_create_qp, qp_init_attr->cap.max_inline_data,
		RPMA_MAX_INLINE_DATA);
}

/*
 * create_qp__peer_NULL -- NULL peer is invalid
 */
static void
create_qp__peer_NULL(void **unused)
{
	/* run test */
	int ret = rpma_peer_setup_qp(NULL, MOCK_CM_ID, MOCK_RPMA_CQ,
			NULL, MOCK_CONN_CFG_DEFAULT);

	/* verify the results */
	assert_int_equal(ret, RPMA_E_INVAL);
}

/*
 * create_qp__id_NULL -- NULL id is invalid
 */
static void
create_qp__id_NULL(void **pprestate)
{
	struct prestate *prestate = *pprestate;

	/* run test */
	int ret = rpma_peer_setup_qp(prestate->peer, NULL, MOCK_RPMA_CQ, NULL,
			MOCK_CONN_CFG_DEFAULT);

	/* verify the results */
	assert_int_equal(ret, RPMA_E_INVAL);
}

/*
 * create_qp__cq_NULL -- NULL cq is invalid
 */
static void
create_qp__cq_NULL(void **pprestate)
{
	struct prestate *prestate = *pprestate;

	/* run test */
	int ret = rpma_peer_setup_qp(prestate->peer, MOCK_CM_ID, NULL,
			NULL, MOCK_CONN_CFG_DEFAULT);

	/* verify the results */
	assert_int_equal(ret, RPMA_E_INVAL);
}

/*
 * create_qp__rdma_create_qp_ERRNO -- rdma_create_qp() fails with MOCK_ERRNO
 */
static void
create_qp__rdma_create_qp_ERRNO(void **pprestate)
{
	struct prestate *prestate = *pprestate;

	for (int i = 0; i < num_rcqs; i++) {
		/* configure mock */
		Get_args.srq = (i == 2) ? MOCK_RPMA_SRQ : NULL;
		configure_create_qp(rcqs[i]);
		will_return(rdma_create_qp, MOCK_ERRNO);

		/* run test */
		int ret = rpma_peer_setup_qp(prestate->peer, MOCK_CM_ID, MOCK_RPMA_CQ,
				rcqs[i], MOCK_CONN_CFG_CUSTOM);

		/* verify the results */
		assert_int_equal(ret, RPMA_E_PROVIDER);
	}
}

/*
 * create_qp__success -- happy day scenario
 */
static void
create_qp__success(void **pprestate)
{
	struct prestate *prestate = *pprestate;

	for (int i = 0; i < num_rcqs; i++) {
		/* configure mock */
		Get_args.srq = (i == 2) ? MOCK_RPMA_SRQ : NULL;
		configure_create_qp(rcqs[i]);
		will_return(rdma_create_qp, MOCK_OK);

		/* run test */
		int ret = rpma_peer_setup_qp(prestate->peer, MOCK_CM_ID, MOCK_RPMA_CQ,
				rcqs[i], MOCK_CONN_CFG_CUSTOM);

		/* verify the results */
		assert_int_equal(ret, MOCK_OK);
	}
}

int
main(int argc, char *argv[])
{
	const struct CMUnitTest tests[] = {
		/* rpma_peer_setup_qp() unit tests */
		cmocka_unit_test(create_qp__peer_NULL),
		cmocka_unit_test_prestate_setup_teardown(create_qp__id_NULL,
				setup__peer, teardown__peer, &prestate_OdpCapable),
		cmocka_unit_test_prestate_setup_teardown(create_qp__cq_NULL,
				setup__peer, teardown__peer, &prestate_OdpCapable),
		cmocka_unit_test_prestate_setup_teardown(
				create_qp__rdma_create_qp_ERRNO,
				setup__peer, teardown__peer, &prestate_OdpCapable),
		cmocka_unit_test_prestate_setup_teardown(create_qp__success,
				setup__peer, teardown__peer, &prestate_OdpCapable),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
